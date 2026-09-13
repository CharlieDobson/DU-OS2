/*===========================================================================
 * ARCCRYP.H  -  Password handling and decryption for XArchive
 * Target: MSVC 2.2  Win32s / Open Watcom 1.9 (DOS/32A, OS/2 PM)
 *
 * Everything the four container backends need in order to read an encrypted
 * entry, and nothing else: XArchive extracts, so this module DECRYPTS.  There
 * is no encryption side and no key generation, which is why there is no RNG
 * here and why AES is only ever used in its decrypting direction - CTR mode
 * being the exception that proves it, since CTR decrypts by encrypting a
 * counter.
 *
 * The block cipher and the hashes are NOT implemented here.  AES.C and SHA.C
 * are verbatim copies of NETSPLOR's TLS crypto stack (S:\CPP\DU32\NSTLS),
 * which is FIPS-197 / FIPS-180 known-answer tested there; they are portable
 * C89 over <string.h> alone, so they drop into the DOS and OS/2 builds
 * unchanged.  Keep them byte-identical to their NSTLS originals - BUILD-NOTES
 * records the cmp check.  What lives HERE is the archive-specific layer on
 * top: the key derivations, which are where the formats differ and where all
 * the folklore is.
 *
 * Four schemes, in ascending order of how much they trust their own maths:
 *
 *   ZipCrypto  - PKWARE's original stream cipher, 1989.  A 96-bit state
 *                driven by CRC-32.  Broken (known plaintext recovers the
 *                state), and we implement it anyway because the archives
 *                exist and the user wants their files, not a lecture.
 *   Zip AES    - WinZip's AE-1/AE-2: PBKDF2-HMAC-SHA1, 1000 iterations,
 *                AES-CTR, HMAC-SHA1 authentication tag.  Carries a two-byte
 *                password check, so a wrong password is known immediately.
 *   7z AES-256 - AES-256-CBC keyed by an iterated SHA-256 over the password.
 *                No check value: a wrong password is only discovered when the
 *                decompressed data fails its CRC.
 *   RAR3       - AES-128-CBC keyed by 262144 rounds of SHA-1, which also
 *                produce the IV a byte at a time.  No check value either.
 *
 * PASSWORD ENCODING.  Three of the four hash the password as UTF-16LE, so the
 * bytes the user typed must be widened, and what a byte MEANS depends on the
 * front end that collected it - see PwToUtf16 below.  ASCII passwords, which
 * is nearly all of them, are identical under every interpretation.
 *===========================================================================*/

#ifndef ARCCRYP_H
#define ARCCRYP_H

#include "arcdefs.h"
#include "sha.h"      /* SHA1_CTX, for the streaming HMAC below */

/*---- Limits -------------------------------------------------------------- */
#define AC_MAX_PW       128     /* characters, not counting the terminator;
                                 * longer than any archiver's own dialog     */
#define AC_MAX_PW_W     ( AC_MAX_PW * 2 )

/*---------------------------------------------------------------------------
 * Password widening.
 *
 * The password arrives as bytes from a DOS text box, an OS/2 entry field or a
 * Win32s edit control, and the formats want UTF-16LE.  For bytes < 0x80 the
 * answer is the same everywhere - widen with a zero high byte - and that is
 * the case that matters, because an archive is usually created on a machine
 * that is not the one opening it, and only ASCII survives that trip reliably.
 *
 * For bytes >= 0x80 there is no single right answer, so we pick the one that
 * matches where the text came from: codepage 437 on DOS and OS/2, where the
 * user typed at a codepage-437 console, and Latin-1 on Win32s, where the edit
 * control hands back CP1252 (identical to Latin-1 across most of the range).
 * Selected at compile time by AC_OEM_PASSWORDS, which the DOS and OS/2 builds
 * define.  Returns the number of UTF-16 code units written.
 *--------------------------------------------------------------------------*/
int  PwToUtf16( const char *pw, Byte *out, int outMax );

/*---- ZipCrypto (traditional PKWARE encryption) ---------------------------- */
typedef struct {
    UInt32 k[3];
} ZipCryptState;

/* Initialise from the password.  ZipCrypto hashes the raw bytes, not UTF-16. */
void ZipCryptInit( ZipCryptState *zc, const char *pw );

/* Decrypt in place.  The first 12 bytes of an encrypted zip entry are an
 * encryption header, not data - decrypt those first, check them with
 * ZipCryptCheck, then carry on through the payload with the same state. */
void ZipCryptDecrypt( ZipCryptState *zc, Byte *buf, UInt32 len );

/*
 * Verify the 12-byte header just decrypted.  Its last byte is a copy of a
 * value the reader already knows, so a wrong password is caught here about
 * 255 times out of 256 rather than at the CRC.  WHICH value depends on how
 * the entry was written:
 *   crc     - the entry's CRC-32, whose high byte is the check byte;
 *   modTime - the DOS time word's high byte, used instead when the entry has
 *             a data descriptor (flag bit 3), because the writer did not yet
 *             know the CRC when it wrote the header.
 * Pass hasDesc to say which.  Returns 1 if the header is plausible.
 *
 * One wrong password in 256 passes this test; that is the format's own odds,
 * not ours, and the CRC catches the rest.
 */
int  ZipCryptCheck( const Byte hdr12[12], UInt32 crc, UInt16 modTime,
                    int hasDesc );

/*---- AES-CTR (WinZip AE-1/AE-2 flavour) ----------------------------------- *
 * CTR with a LITTLE-endian counter that starts at 1 - not the big-endian one
 * from SP800-38A that TLS and GCM use.  Getting this backwards decrypts the
 * first block perfectly and turns everything after it into noise, which is a
 * memorable way to lose an afternoon.
 *--------------------------------------------------------------------------*/
typedef struct {
    Byte rk[240];       /* round keys: 176 used for AES-128, 240 for AES-256 */
    int  nr;            /* 10 or 14                                          */
    Byte ctr[16];       /* the counter block, low bytes first                */
    Byte pad[16];       /* current key-stream block                          */
    int  used;          /* bytes of pad already consumed (16 = exhausted)    */
} AesCtrState;

/* keyBytes must be 16 or 32.  Returns SZ_OK, or SZ_ERR_UNSUPPORTED for a key
 * size we have no schedule for - see the AES-192 note in ARCCRYP.C. */
int  AesCtrInit( AesCtrState *s, const Byte *key, int keyBytes );

/* XOR the key stream over buf.  Call repeatedly to stream a whole entry: the
 * counter and the partially-used pad carry across calls, so the split between
 * calls need not line up with the 16-byte blocks. */
void AesCtrXor( AesCtrState *s, Byte *buf, UInt32 len );

/*---- AES-CBC decryption (7z and RAR3) ------------------------------------- */
typedef struct {
    Byte rk[240];
    int  nr;
    Byte iv[16];        /* running: the last ciphertext block seen           */
} AesCbcState;

int  AesCbcInit( AesCbcState *s, const Byte *key, int keyBytes,
                 const Byte iv[16] );

/* Decrypt in place.  len must be a multiple of 16; the IV carries across calls
 * so a stream can be decrypted in chunks. */
void AesCbcDecrypt( AesCbcState *s, Byte *buf, UInt32 len );

/*---- Streaming HMAC-SHA1 -------------------------------------------------- *
 * SHA.C offers HMAC only as a one-shot over a buffer in memory, which is no
 * use for authenticating an archive entry: the whole point is that the entry
 * is streamed and never wholly in memory at once.
 *
 * This matters more than it looks.  WinZip AE-2 does not store a CRC - the
 * field is written as zero - so for an AE-2 entry this ten-byte tag is the
 * ONLY integrity check that exists.  Skip it and a corrupt AE-2 entry extracts
 * silently as garbage, which is the one outcome an extractor must never have.
 *--------------------------------------------------------------------------*/
typedef struct {
    SHA1_CTX ctx;               /* running inner hash                       */
    Byte     opad[64];          /* kept for the outer hash at the end       */
} HmacSha1Ctx;

void HmacSha1Init( HmacSha1Ctx *h, const Byte *key, int keyLen );
void HmacSha1Update( HmacSha1Ctx *h, const void *data, unsigned len );
void HmacSha1Final( HmacSha1Ctx *h, Byte out[20] );

/*---- PBKDF2 (RFC 2898) ---------------------------------------------------- */
void Pbkdf2Sha1( const Byte *pw, int pwLen, const Byte *salt, int saltLen,
                 UInt32 iter, Byte *out, int outLen );
void Pbkdf2Sha256( const Byte *pw, int pwLen, const Byte *salt, int saltLen,
                   UInt32 iter, Byte *out, int outLen );

/*---- Per-format key derivation -------------------------------------------- */

/*
 * WinZip AES.  strength is the byte from the 0x9901 extra field:
 *   1 = AES-128 (8-byte salt), 2 = AES-192 (12), 3 = AES-256 (16).
 * Writes the cipher key, the HMAC-SHA1 key (both *keyBytes long) and the
 * two-byte password verifier.  Returns SZ_ERR_UNSUPPORTED for strength 2 -
 * see the AES-192 note in ARCCRYP.C.
 */
int  ZipAesDeriveKeys( const char *pw, const Byte *salt, int strength,
                       Byte *cipherKey, Byte *macKey, Byte pwVerify[2],
                       int *keyBytes );

/* Salt length for a given strength, or 0 if the strength byte is not 1..3. */
int  ZipAesSaltLen( int strength );

/*
 * 7z AES-256.  numCyclesPower is the low 6 bits of the coder's first property
 * byte: the key is 2^numCyclesPower rounds of SHA-256, so 19 (the 7-Zip
 * default) is half a million hashes and is SLOW BY DESIGN - roughly a second
 * on a 486.  The special value 0x3F means "no hashing at all", a debugging
 * mode nothing writes; we honour it because it costs three lines.
 */
void SzAesDeriveKey( const char *pw, int numCyclesPower,
                     const Byte *salt, int saltLen, Byte key[32] );

/*---- RAR5 -----------------------------------------------------------------*
 * AES-256-CBC, keyed by PBKDF2-HMAC-SHA256 over the password as UTF-8.  The
 * one structural oddity is that a SINGLE PBKDF2 chain yields three values
 * rather than one, by taking snapshots as it runs:
 *
 *     after 2^kdfCount iterations        -> the AES key
 *     after 16 more                      -> the HMAC key (see the tweak below)
 *     after 16 more                      -> the password check value
 *
 * Running three separate PBKDF2s would give three different answers and is the
 * obvious way to get this wrong, because the expensive part looks like it
 * ought to be shared rather than continued.
 *
 * pswCheck is the 32-byte third snapshot folded down to 8 bytes by XOR; it is
 * what lets a wrong RAR5 password be rejected before a byte is decrypted,
 * which neither 7z nor RAR3 can do.
 *--------------------------------------------------------------------------*/
void Rar5DeriveKeys( const char *pw, const Byte salt[16], int kdfCount,
                     Byte key[32], Byte hashKey[32], Byte pswCheck[8] );

/*
 * RAR5 "tweaked" checksums.
 *
 * When an entry is encrypted, RAR5 may store not the data's CRC-32 but an HMAC
 * of it, folded to 32 bits - because a plain checksum of the PLAINTEXT, sitting
 * in cleartext in the header, is a crib an attacker can test candidate
 * passwords against.  Given the CRC actually computed over the decrypted data,
 * this returns the value the header should hold.
 *
 * Only applied when the crypt record asks for it; an encrypted entry without
 * that flag stores an ordinary CRC.
 */
UInt32 Rar5TweakCrc( UInt32 crc, const Byte hashKey[32] );

/*
 * RAR3 (RAR 2.9 / 3.x / 4.x) AES-128-CBC key derivation.
 *
 * 262144 rounds of SHA-1 over (password as UTF-16LE ++ salt ++ a 3-byte round
 * counter) in one running hash; the final digest is the key with each 4-byte
 * word byte-reversed, and sixteen snapshots taken every 16384 rounds give the
 * sixteen IV bytes (digest[19] from each).
 *
 * The hash is SHA-1 with a deliberate bug carried over from RAR 2.9 - see the
 * long comment in ARCCRYP.C.  It only bites once the password passes 28
 * characters, so a correct SHA-1 gets ordinary archives right and then fails
 * silently on a long password; both regimes are verified against WinRAR 5.0
 * archives at password lengths 4 through 100.
 *
 * Unlike RAR5 there is NO password check value, so a wrong password cannot be
 * detected up front - it is inferred afterwards from a failed CRC, exactly as
 * for 7z.  See the SZ_ERR_PASSWORD / SZ_ERR_BADPASS note in ARCDEFS.H.
 */
void Rar3DeriveKeys( const char *pw, const Byte salt[8],
                     Byte key[16], Byte iv[16] );

#endif /* ARCCRYP_H */
