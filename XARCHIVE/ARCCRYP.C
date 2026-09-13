/*===========================================================================
 * ARCCRYP.C  -  Password handling and decryption for XArchive
 * Target: MSVC 2.2  Win32s / Open Watcom 1.9 (DOS/32A, OS/2 PM)
 *
 * See ARCCRYP.H for what this module is and is not.  The short version: the
 * primitives come from NETSPLOR's crypto stack (AES.C, SHA.C - copied here
 * verbatim and known-answer tested there), and what is written below is the
 * archive-specific layer, which is mostly key derivation.
 *
 * A note on why the derivations look so different from each other.  Each was
 * designed at a different time against a different idea of what an attacker
 * could do, and each is frozen into archives that already exist, so none of
 * them can be improved - only implemented exactly:
 *
 *   1989  ZipCrypto  no key derivation at all; the password bytes are stirred
 *                    straight into a 96-bit CRC-driven state.
 *   2003  Zip AES    PBKDF2 with 1000 iterations, which was a reasonable
 *                    number in 2003 and is a rounding error now.
 *   2004  7z AES     2^19 rounds of SHA-256 over salt+password+counter.  Not
 *                    PBKDF2, not HMAC, just a very long hash - and the reason
 *                    opening a 7z asks for the password ONCE and then pauses.
 *
 * All three are deliberately slow to key and fast to run, so every derivation
 * here is computed once per archive and cached by the caller, never per entry.
 *===========================================================================*/

#include "arccryp.h"
#include "crc32.h"
#include "aes.h"
#include "sha.h"
#include <string.h>

/*===========================================================================
 * Password widening
 *===========================================================================*/

/*
 * Codepage 437 -> Unicode for the high half.  This is the table a DOS or OS/2
 * user's keyboard actually produces, and it is NOT Latin-1: the two agree on
 * almost nothing above 0x7F, because CP437 spends most of its high half on box
 * drawing.  An archive whose password contains, say, an e-acute is only
 * portable between machines that agree on this mapping, which is exactly why
 * every archiver's documentation quietly suggests sticking to ASCII.
 */
#ifdef AC_OEM_PASSWORDS
static const UInt16 g_highMap[128] = {
/* 80 */ 0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,
/* 88 */ 0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
/* 90 */ 0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,
/* 98 */ 0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
/* A0 */ 0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,
/* A8 */ 0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
/* B0 */ 0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,
/* B8 */ 0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
/* C0 */ 0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,
/* C8 */ 0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
/* D0 */ 0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,
/* D8 */ 0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
/* E0 */ 0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,
/* E8 */ 0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
/* F0 */ 0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,
/* F8 */ 0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0
};
#endif

int PwToUtf16( const char *pw, Byte *out, int outMax )
{
    const Byte *p = (const Byte *)pw;
    int         n = 0;

    while ( *p && n < outMax )
    {
        UInt16 u;

        if ( *p < 0x80 )
            u = (UInt16)*p;
        else
#ifdef AC_OEM_PASSWORDS
            u = g_highMap[ *p - 0x80 ];
#else
            u = (UInt16)*p;         /* Latin-1 / CP1252: the byte IS the code
                                     * point across the range that matters   */
#endif

        out[ n * 2 + 0 ] = (Byte)( u & 0xFF );
        out[ n * 2 + 1 ] = (Byte)( u >> 8 );
        n++;
        p++;
    }

    return n;
}

/*===========================================================================
 * ZipCrypto - PKWARE traditional encryption
 *
 * Three 32-bit registers stirred by CRC-32.  Note that the CRC step here is
 * the RAW table step with no pre- or post-inversion, which is exactly what
 * Crc32Update does for a single byte: table[(crc ^ b) & 0xFF] ^ (crc >> 8).
 * Reusing it rather than copying the table in is not just tidiness - it means
 * there is one CRC table in the image, which on a 286-era DOS build is 1K of
 * real estate worth caring about.
 *===========================================================================*/

static void ZcUpdate( ZipCryptState *zc, Byte c )
{
    Byte t;

    zc->k[0] = Crc32Update( zc->k[0], &c, 1 );
    zc->k[1] = ( zc->k[1] + ( zc->k[0] & 0xFF ) ) * 134775813UL + 1;
    t        = (Byte)( zc->k[1] >> 24 );
    zc->k[2] = Crc32Update( zc->k[2], &t, 1 );
}

void ZipCryptInit( ZipCryptState *zc, const char *pw )
{
    const Byte *p = (const Byte *)pw;

    zc->k[0] = 0x12345678UL;
    zc->k[1] = 0x23456789UL;
    zc->k[2] = 0x34567890UL;

    while ( *p )
        ZcUpdate( zc, *p++ );
}

/* The key-stream byte, from the low 16 bits of k[2]. */
static Byte ZcByte( ZipCryptState *zc )
{
    UInt16 temp = (UInt16)( ( zc->k[2] | 2 ) & 0xFFFF );

    return (Byte)( ( ( UInt32)temp * ( temp ^ 1 ) ) >> 8 );
}

void ZipCryptDecrypt( ZipCryptState *zc, Byte *buf, UInt32 len )
{
    while ( len-- )
    {
        /* Decrypt, THEN stir the plaintext back in - the state advances on
         * what the file said, not on what was on the disk. */
        *buf = (Byte)( *buf ^ ZcByte( zc ) );
        ZcUpdate( zc, *buf );
        buf++;
    }
}

int ZipCryptCheck( const Byte hdr12[12], UInt32 crc, UInt16 modTime,
                   int hasDesc )
{
    Byte want = hasDesc ? (Byte)( modTime >> 8 )
                        : (Byte)( crc >> 24 );

    return hdr12[11] == want;
}

/*===========================================================================
 * AES-CTR, WinZip flavour
 *===========================================================================*/

/*
 * AES-192 is missing on purpose.  NETSPLOR's AES.C provides the 128- and
 * 256-bit key schedules and nothing else, and adding a 192-bit one HERE would
 * mean a second copy of the S-box in the image, because AES.C keeps its own
 * static.  Since 7-Zip and WinZip both write AES-256 by default and 192 has to
 * be asked for deliberately, the trade is not worth it: we refuse strength 2
 * with a precise message rather than carrying 256 bytes of duplicate table for
 * an archive nobody has.  If one ever turns up, the honest fix is to export
 * the schedule from AES.C, not to duplicate the table.
 */
static int RkExpand( Byte *rk, int *nr, const Byte *key, int keyBytes )
{
    if ( keyBytes == 16 )
    {
        aes128_expand_key( key, rk );
        *nr = 10;
        return SZ_OK;
    }
    if ( keyBytes == 32 )
    {
        aes256_expand_key( key, rk );
        *nr = 14;
        return SZ_OK;
    }
    return SZ_ERR_UNSUPPORTED;
}

int AesCtrInit( AesCtrState *s, const Byte *key, int keyBytes )
{
    int rc = RkExpand( s->rk, &s->nr, key, keyBytes );

    if ( rc != SZ_OK )
        return rc;

    memset( s->ctr, 0, 16 );
    memset( s->pad, 0, 16 );
    s->used = 16;               /* forces a fresh block, so the FIRST counter
                                 * value used is 1 and not 0                 */
    return SZ_OK;
}

void AesCtrXor( AesCtrState *s, Byte *buf, UInt32 len )
{
    while ( len-- )
    {
        if ( s->used >= 16 )
        {
            int i = 0;

            /* Increment as a little-endian 128-bit integer.  Only the first
             * byte ever moves for files under 4GB, but the carry chain is
             * three lines and saves having to think about it again. */
            while ( i < 16 && ++s->ctr[i] == 0 )
                i++;

            aes_encrypt( s->rk, s->nr, s->ctr, s->pad );
            s->used = 0;
        }

        *buf++ ^= s->pad[ s->used++ ];
    }
}

/*===========================================================================
 * AES-CBC decryption
 *
 * AES.C has an aes_cbc_decrypt, but it takes the IV by value and cannot carry
 * it from one call to the next, so it can only do a whole buffer at once.  A
 * 7z folder can be larger than memory, so we need the streaming form.
 *===========================================================================*/

int AesCbcInit( AesCbcState *s, const Byte *key, int keyBytes,
                const Byte iv[16] )
{
    int rc = RkExpand( s->rk, &s->nr, key, keyBytes );

    if ( rc != SZ_OK )
        return rc;

    memcpy( s->iv, iv, 16 );
    return SZ_OK;
}

void AesCbcDecrypt( AesCbcState *s, Byte *buf, UInt32 len )
{
    Byte   prev[16];
    UInt32 off;

    for ( off = 0; off + 16 <= len; off += 16 )
    {
        int i;

        memcpy( prev, buf + off, 16 );          /* this block feeds the next */
        aes_decrypt( s->rk, s->nr, buf + off, buf + off );
        for ( i = 0; i < 16; i++ )
            buf[ off + i ] ^= s->iv[i];
        memcpy( s->iv, prev, 16 );
    }
}

/*===========================================================================
 * Streaming HMAC-SHA1 (RFC 2104)
 *
 *   HMAC(K, m) = H( (K ^ opad) || H( (K ^ ipad) || m ) )
 *
 * The inner hash runs as the data streams past; the outer one waits in opad
 * until the end.  Keys longer than the 64-byte block are hashed first - which
 * never happens here, since every key we pass is 16 or 32 bytes, but leaving
 * the case out would make this subtly not-HMAC and someone would reuse it.
 *===========================================================================*/

void HmacSha1Init( HmacSha1Ctx *h, const Byte *key, int keyLen )
{
    Byte k[64], ipad[64];
    int  i;

    memset( k, 0, 64 );
    if ( keyLen > 64 )
        sha1( key, (unsigned)keyLen, k );       /* long key: hash it down */
    else
        memcpy( k, key, keyLen );

    for ( i = 0; i < 64; i++ )
    {
        ipad[i]    = (Byte)( k[i] ^ 0x36 );
        h->opad[i] = (Byte)( k[i] ^ 0x5C );
    }

    sha1_init( &h->ctx );
    sha1_update( &h->ctx, ipad, 64 );
}

void HmacSha1Update( HmacSha1Ctx *h, const void *data, unsigned len )
{
    sha1_update( &h->ctx, data, len );
}

void HmacSha1Final( HmacSha1Ctx *h, Byte out[20] )
{
    Byte     inner[20];
    SHA1_CTX o;

    sha1_final( &h->ctx, inner );
    sha1_init( &o );
    sha1_update( &o, h->opad, 64 );
    sha1_update( &o, inner, 20 );
    sha1_final( &o, out );
}

/*===========================================================================
 * PBKDF2 (RFC 2898)
 *===========================================================================*/

#define AC_MAX_SALT  64         /* the formats use 8..16; 64 is headroom     */

static void Pbkdf2Core( int sha256, const Byte *pw, int pwLen,
                        const Byte *salt, int saltLen,
                        UInt32 iter, Byte *out, int outLen )
{
    Byte   sb[ AC_MAX_SALT + 4 ];
    Byte   U[32], Un[32], T[32];
    int    hLen = sha256 ? 32 : 20;
    UInt32 blk  = 1;

    if ( saltLen > AC_MAX_SALT )
        saltLen = AC_MAX_SALT;
    memcpy( sb, salt, saltLen );

    while ( outLen > 0 )
    {
        UInt32 j;
        int    i, n;

        /* INT(blk) is big-endian, even though everything else in these
         * formats is little-endian.  RFC 2898 says network order and the
         * archivers followed it. */
        sb[ saltLen + 0 ] = (Byte)( blk >> 24 );
        sb[ saltLen + 1 ] = (Byte)( blk >> 16 );
        sb[ saltLen + 2 ] = (Byte)( blk >> 8  );
        sb[ saltLen + 3 ] = (Byte)( blk       );

        if ( sha256 )
            hmac_sha256( pw, pwLen, sb, saltLen + 4, U );
        else
            hmac_sha1( pw, pwLen, sb, saltLen + 4, U );
        memcpy( T, U, hLen );

        for ( j = 1; j < iter; j++ )
        {
            /* Into a separate buffer: hmac_* makes no promise that its output
             * may alias its input, and relying on one that happens to hold
             * today is how a crypto bug gets planted for later. */
            if ( sha256 )
                hmac_sha256( pw, pwLen, U, hLen, Un );
            else
                hmac_sha1( pw, pwLen, U, hLen, Un );
            memcpy( U, Un, hLen );

            for ( i = 0; i < hLen; i++ )
                T[i] ^= U[i];
        }

        n = ( outLen < hLen ) ? outLen : hLen;
        memcpy( out, T, n );
        out    += n;
        outLen -= n;
        blk++;
    }
}

void Pbkdf2Sha1( const Byte *pw, int pwLen, const Byte *salt, int saltLen,
                 UInt32 iter, Byte *out, int outLen )
{
    Pbkdf2Core( 0, pw, pwLen, salt, saltLen, iter, out, outLen );
}

void Pbkdf2Sha256( const Byte *pw, int pwLen, const Byte *salt, int saltLen,
                   UInt32 iter, Byte *out, int outLen )
{
    Pbkdf2Core( 1, pw, pwLen, salt, saltLen, iter, out, outLen );
}

/*===========================================================================
 * WinZip AES key derivation
 *===========================================================================*/

int ZipAesSaltLen( int strength )
{
    switch ( strength )
    {
        case 1: return 8;       /* AES-128 */
        case 2: return 12;      /* AES-192 */
        case 3: return 16;      /* AES-256 */
    }
    return 0;
}

int ZipAesDeriveKeys( const char *pw, const Byte *salt, int strength,
                      Byte *cipherKey, Byte *macKey, Byte pwVerify[2],
                      int *keyBytes )
{
    Byte material[ 32 + 32 + 2 ];
    int  kb, total;

    if ( strength == 1 )
        kb = 16;
    else if ( strength == 3 )
        kb = 32;
    else
        return SZ_ERR_UNSUPPORTED;      /* 2 = AES-192, see RkExpand above */

    total = kb * 2 + 2;

    /* Zip AES hashes the password as raw BYTES, not UTF-16 - the one scheme
     * of the three that does, which is why PwToUtf16 is not called here. */
    Pbkdf2Sha1( (const Byte *)pw, (int)strlen( pw ),
                salt, ZipAesSaltLen( strength ), 1000, material, total );

    memcpy( cipherKey, material,      kb );
    memcpy( macKey,    material + kb, kb );
    pwVerify[0] = material[ kb * 2 + 0 ];
    pwVerify[1] = material[ kb * 2 + 1 ];
    *keyBytes   = kb;

    return SZ_OK;
}

/*===========================================================================
 * 7z AES-256 key derivation
 *
 * Not PBKDF2 and not HMAC: one SHA-256 context is fed 2^numCyclesPower copies
 * of (salt || passwordUTF16LE || counter), where the counter is a 64-bit
 * little-endian round number, and the final digest is the key.  The cost is
 * the point - at the 7-Zip default of 19 that is 524288 rounds.
 *===========================================================================*/

void SzAesDeriveKey( const char *pw, int numCyclesPower,
                     const Byte *salt, int saltLen, Byte key[32] )
{
    Byte pwW[ AC_MAX_PW_W ];
    int  pwLen = PwToUtf16( pw, pwW, AC_MAX_PW ) * 2;

    if ( numCyclesPower == 0x3F )
    {
        /* "No derivation": salt then password, zero padded.  Nothing writes
         * this, but the format defines it and honouring it is three lines. */
        int i, pos = 0;

        memset( key, 0, 32 );
        for ( i = 0; i < saltLen && pos < 32; i++ )
            key[ pos++ ] = salt[i];
        for ( i = 0; i < pwLen && pos < 32; i++ )
            key[ pos++ ] = pwW[i];
        return;
    }

    {
        SHA256_CTX c;
        Byte       ctr[8];
        UInt32     i, rounds;

        sha256_init( &c );
        memset( ctr, 0, 8 );

        /* numCyclesPower is six bits, so in principle up to 2^62 rounds - a
         * number no machine will finish.  The caller refuses anything absurd
         * before we get here (see SZARC.C); the clamp is a backstop so a
         * malformed header cannot hang the program outright. */
        rounds = ( numCyclesPower >= 31 ) ? 0x80000000UL
                                          : ( 1UL << numCyclesPower );

        for ( i = 0; i < rounds; i++ )
        {
            int k;

            sha256_update( &c, salt, (unsigned)saltLen );
            sha256_update( &c, pwW,  (unsigned)pwLen );
            sha256_update( &c, ctr,  8 );

            for ( k = 0; k < 8; k++ )       /* ++counter, little-endian */
                if ( ++ctr[k] != 0 )
                    break;
        }

        sha256_final( &c, key );
    }
}

/*===========================================================================
 * RAR5 key derivation
 *
 * PBKDF2-HMAC-SHA256, but with a twist that has to be read carefully: the
 * three outputs are SNAPSHOTS OF ONE RUNNING CHAIN, not three separate
 * derivations.  The chain runs 2^kdfCount iterations and the key is taken;
 * it then runs 16 more and the HMAC key is taken; 16 more again and the
 * password check value is taken.
 *
 * Doing three independent PBKDF2s would be the natural reading, would cost
 * three times as much, and would produce three wrong answers.
 *===========================================================================*/

void Rar5DeriveKeys( const char *pw, const Byte salt[16], int kdfCount,
                     Byte key[32], Byte hashKey[32], Byte pswCheck[8] )
{
    Byte   sb[ 16 + 4 ];
    Byte   U[32], Un[32], Fn[32];
    Byte   check[32];
    UInt32 counts[3];
    Byte  *outs[3];
    int    pwLen = (int)strlen( pw );
    int    i, k;

    memcpy( sb, salt, 16 );
    sb[16] = 0; sb[17] = 0; sb[18] = 0; sb[19] = 1;   /* INT(1), big-endian */

    hmac_sha256( (const Byte *)pw, pwLen, sb, 20, U );
    memcpy( Fn, U, 32 );                    /* the chain after one iteration */

    /* The first leg is 2^kdfCount MINUS ONE more, because the iteration above
     * already counts as the first.  kdfCount is a log: WinRAR writes 15 by
     * default, so this is 32768 rounds and not 15. */
    {
        UInt32 total = ( kdfCount >= 24 ) ? ( 1UL << 24 ) : ( 1UL << kdfCount );
        counts[0] = ( total > 0 ) ? total - 1 : 0;
    }
    counts[1] = 16;
    counts[2] = 16;
    outs[0]   = key;
    outs[1]   = hashKey;
    outs[2]   = check;

    for ( i = 0; i < 3; i++ )
    {
        UInt32 j;

        for ( j = 0; j < counts[i]; j++ )
        {
            hmac_sha256( (const Byte *)pw, pwLen, U, 32, Un );
            memcpy( U, Un, 32 );
            for ( k = 0; k < 32; k++ )
                Fn[k] ^= U[k];
        }
        memcpy( outs[i], Fn, 32 );
    }

    /* The check value is the 32-byte snapshot folded into 8 by XOR. */
    memset( pswCheck, 0, 8 );
    for ( i = 0; i < 32; i++ )
        pswCheck[ i & 7 ] ^= check[i];
}

UInt32 Rar5TweakCrc( UInt32 crc, const Byte hashKey[32] )
{
    Byte   raw[4], digest[32];
    UInt32 out = 0;
    int    i;

    /* LITTLE-endian, like the rest of RAR5 - unrar spells this RawPut4.  It
     * was written big-endian here first, on the reasoning that a value being
     * fed to a hash is usually in network order; the result was a transform
     * that was wrong in a way nothing else could reveal, because the decrypted
     * DATA was already perfect and only the checksum comparison failed. */
    raw[0] = (Byte)( crc       );
    raw[1] = (Byte)( crc >> 8  );
    raw[2] = (Byte)( crc >> 16 );
    raw[3] = (Byte)( crc >> 24 );

    hmac_sha256( hashKey, 32, raw, 4, digest );

    for ( i = 0; i < 32; i++ )
        out ^= (UInt32)digest[i] << ( ( i & 3 ) * 8 );

    return out;
}

/*===========================================================================
 * RAR3 (RAR 2.9 / 3.x / 4.x) key derivation - AES-128-CBC
 *
 * 262144 rounds of SHA-1 over (password as UTF-16LE ++ 8-byte salt ++ a
 * 3-byte little-endian round counter), all in ONE running hash.  The final
 * digest is the AES key; sixteen snapshots taken every 16384 rounds give the
 * sixteen IV bytes, one byte each.
 *
 * Two details are pure convention and cannot be guessed from the shape of the
 * algorithm - both were settled by decrypting real WinRAR archives:
 *
 *   - the AES key is the digest with each 4-byte word BYTE-REVERSED, not the
 *     digest as it comes out of SHA-1;
 *   - each IV byte is digest[19], the LOW byte of the last state word.
 *
 * And then there is the famous part.  RAR 2.9 hashes with a SHA-1 that has a
 * BUG in it, and the bug is load-bearing: WinRAR has to keep producing it
 * forever or old archives stop opening.  Its transform expands the message
 * schedule IN PLACE in a 16-word circular buffer (the Steve Reid public-domain
 * SHA-1, built without SHA1HANDSOFF), so when the transform is handed the
 * CALLER's buffer it hands it back containing the last sixteen schedule words
 * instead of the data that went in.  Because every round re-hashes the same
 * password buffer, each corruption feeds the next round.
 *
 * Whether that ever fires depends on the buffering, which is the part that is
 * genuinely easy to get wrong:
 *
 *   - a block is only ever corrupted if the transform reads it DIRECTLY from
 *     the password buffer;
 *   - data that gets assembled in the hash's own 64-byte buffer first is safe,
 *     and this implementation (like the original) fills that buffer whenever
 *     it can, even when it is empty.
 *
 * So a direct block needs more than a full block LEFT OVER after topping up
 * the internal buffer.  With 2*passwordLen+8 bytes per round that cannot
 * happen at all until the password passes 28 characters - which is exactly why
 * a plain, correct SHA-1 decrypts ordinary RAR3 archives perfectly and then
 * silently produces garbage for a long password.  Both halves were verified
 * against WinRAR-written archives at 15 password lengths from 4 to 100
 * characters; a correct SHA-1 matches to 28 and diverges from 29 on.
 *===========================================================================*/

#define R3_ROUNDS      0x40000      /* 262144                               */
#define R3_SNAPSHOTS   16           /* one IV byte each                     */
#define R3_MAXPW       127          /* WinRAR's own limit                   */

typedef struct {
    UInt32 h[5];
    UInt32 lenLo, lenHi;            /* message length in BITS               */
    Byte   buf[64];
    int    nbuf;
} Rar3Sha;

#define R3ROL(x,n)  ( ( (x) << (n) ) | ( (x) >> ( 32 - (n) ) ) )

/* One SHA-1 block.  When 'writeBack' is set, p is the caller's own buffer and
 * the last sixteen schedule words are written back over it - see above. */
static void Rar3ShaBlock( Rar3Sha *c, Byte *p, int writeBack )
{
    UInt32 w[80], a, b, d, e, f, k, t;
    int    i;

    for ( i = 0; i < 16; i++ )
        w[i] = ( (UInt32)p[i*4] << 24 ) | ( (UInt32)p[i*4+1] << 16 ) |
               ( (UInt32)p[i*4+2] << 8 ) | (UInt32)p[i*4+3];

    for ( i = 16; i < 80; i++ )
    {
        t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = R3ROL( t, 1 );
    }

    a = c->h[0]; b = c->h[1]; d = c->h[2]; e = c->h[3]; f = c->h[4];

    for ( i = 0; i < 80; i++ )
    {
        if      ( i < 20 ) { k = 0x5A827999UL; t = ( b & d ) | ( ~b & e ); }
        else if ( i < 40 ) { k = 0x6ED9EBA1UL; t = b ^ d ^ e; }
        else if ( i < 60 ) { k = 0x8F1BBCDCUL; t = ( b & d ) | ( b & e ) | ( d & e ); }
        else               { k = 0xCA62C1D6UL; t = b ^ d ^ e; }
        t += R3ROL( a, 5 ) + f + k + w[i];
        f = e; e = d; d = R3ROL( b, 30 ); b = a; a = t;
    }

    c->h[0] += a; c->h[1] += b; c->h[2] += d; c->h[3] += e; c->h[4] += f;

    /* The bug, reproduced deliberately: w[64..79] is what an in-place circular
     * schedule leaves behind, stored in the host's little-endian order. */
    if ( writeBack )
        for ( i = 0; i < 16; i++ )
        {
            UInt32 v = w[64+i];
            p[i*4+0] = (Byte)( v       );
            p[i*4+1] = (Byte)( v >> 8  );
            p[i*4+2] = (Byte)( v >> 16 );
            p[i*4+3] = (Byte)( v >> 24 );
        }
}

static void Rar3ShaInit( Rar3Sha *c )
{
    c->h[0] = 0x67452301UL; c->h[1] = 0xEFCDAB89UL; c->h[2] = 0x98BADCFEUL;
    c->h[3] = 0x10325476UL; c->h[4] = 0xC3D2E1F0UL;
    c->lenLo = c->lenHi = 0;
    c->nbuf  = 0;
}

/* 'direct' says the caller's buffer is one this hash is allowed to corrupt;
 * it is 0 for the padding inside Rar3ShaFinal, which must not be touched. */
static void Rar3ShaUpdate( Rar3Sha *c, Byte *data, int len, int direct )
{
    int fill;

    c->lenLo += (UInt32)len << 3;
    if ( c->lenLo < ( (UInt32)len << 3 ) ) c->lenHi++;
    c->lenHi += (UInt32)len >> 29;

    /* Top the internal buffer up FIRST, even when it is empty.  That single
     * choice is what keeps short passwords on the correct-SHA-1 path. */
    fill = 64 - c->nbuf;
    if ( len >= fill )
    {
        memcpy( c->buf + c->nbuf, data, (unsigned)fill );
        Rar3ShaBlock( c, c->buf, 0 );
        data += fill; len -= fill; c->nbuf = 0;
    }
    while ( len >= 64 )
    {
        Rar3ShaBlock( c, data, direct );
        data += 64; len -= 64;
    }
    if ( len )
    {
        memcpy( c->buf + c->nbuf, data, (unsigned)len );
        c->nbuf += len;
    }
}

static void Rar3ShaFinal( Rar3Sha *c, Byte out[20] )
{
    Byte   pad[136];
    int    n, i;
    UInt32 lo = c->lenLo, hi = c->lenHi;

    n = ( c->nbuf < 56 ) ? ( 56 - c->nbuf ) : ( 120 - c->nbuf );
    memset( pad, 0, sizeof( pad ) );
    pad[0] = 0x80;
    Rar3ShaUpdate( c, pad, n, 0 );
    c->lenLo = lo; c->lenHi = hi;       /* padding is not message length */

    pad[0] = (Byte)( hi >> 24 ); pad[1] = (Byte)( hi >> 16 );
    pad[2] = (Byte)( hi >> 8  ); pad[3] = (Byte)( hi       );
    pad[4] = (Byte)( lo >> 24 ); pad[5] = (Byte)( lo >> 16 );
    pad[6] = (Byte)( lo >> 8  ); pad[7] = (Byte)( lo       );
    Rar3ShaUpdate( c, pad, 8, 0 );

    for ( i = 0; i < 5; i++ )
    {
        out[i*4+0] = (Byte)( c->h[i] >> 24 );
        out[i*4+1] = (Byte)( c->h[i] >> 16 );
        out[i*4+2] = (Byte)( c->h[i] >> 8  );
        out[i*4+3] = (Byte)( c->h[i]       );
    }
}

void Rar3DeriveKeys( const char *pw, const Byte salt[8],
                     Byte key[16], Byte iv[16] )
{
    Byte    raw[ R3_MAXPW * 2 + 8 ];
    Byte    digest[20], num[3];
    Rar3Sha c, snap;
    int     rawLen, chars, i, j;

    chars  = PwToUtf16( pw, raw, R3_MAXPW );
    rawLen = chars * 2;
    memcpy( raw + rawLen, salt, 8 );
    rawLen += 8;

    Rar3ShaInit( &c );
    for ( i = 0; i < R3_ROUNDS; i++ )
    {
        Rar3ShaUpdate( &c, raw, rawLen, 1 );

        num[0] = (Byte)( i       );
        num[1] = (Byte)( i >> 8  );
        num[2] = (Byte)( i >> 16 );
        Rar3ShaUpdate( &c, num, 3, 1 );

        if ( ( i % ( R3_ROUNDS / R3_SNAPSHOTS ) ) == 0 )
        {
            snap = c;                       /* the running hash CONTINUES */
            Rar3ShaFinal( &snap, digest );
            iv[ i / ( R3_ROUNDS / R3_SNAPSHOTS ) ] = digest[19];
        }
    }
    Rar3ShaFinal( &c, digest );

    for ( i = 0; i < 4; i++ )
        for ( j = 0; j < 4; j++ )
            key[ i*4 + j ] = digest[ i*4 + 3 - j ];
}
