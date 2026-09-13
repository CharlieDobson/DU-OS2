/* ==========================================================================
 * AES.C - AES-128 (FIPS-197) + CBC mode (SP800-38A).  Portable C89.
 * ==========================================================================
 * State is held as 16 bytes in column-major order: byte (row r, col c) lives
 * at index 4*c + r, exactly as in FIPS-197.  Reference implementation; the
 * inner rounds are the natural target for a hand-asm port later.
 * ========================================================================== */
#include "aes.h"
#include <string.h>

static const unsigned char sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const unsigned char inv_sbox[256] = {
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const unsigned char rcon[11] = {
0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

/* Multiply by x (i.e. by 2) in GF(2^8) with the AES reduction polynomial. */
static unsigned char xtime(unsigned char x)
{
    return (unsigned char)((x << 1) ^ (((x >> 7) & 1) * 0x1b));
}

/* General GF(2^8) multiply (used only by the inverse MixColumns). */
static unsigned char gmul(unsigned char a, unsigned char b)
{
    unsigned char p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1)
            p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

void aes128_expand_key(const unsigned char key[16], unsigned char rk[176])
{
    int i;
    unsigned char t0, t1, t2, t3, tmp;

    memcpy(rk, key, 16);
    for (i = 4; i < 44; i++) {
        t0 = rk[(i-1)*4 + 0];
        t1 = rk[(i-1)*4 + 1];
        t2 = rk[(i-1)*4 + 2];
        t3 = rk[(i-1)*4 + 3];
        if ((i & 3) == 0) {                     /* RotWord + SubWord + Rcon */
            tmp = t0;
            t0 = (unsigned char)(sbox[t1] ^ rcon[i/4]);
            t1 = sbox[t2];
            t2 = sbox[t3];
            t3 = sbox[tmp];
        }
        rk[i*4 + 0] = (unsigned char)(rk[(i-4)*4 + 0] ^ t0);
        rk[i*4 + 1] = (unsigned char)(rk[(i-4)*4 + 1] ^ t1);
        rk[i*4 + 2] = (unsigned char)(rk[(i-4)*4 + 2] ^ t2);
        rk[i*4 + 3] = (unsigned char)(rk[(i-4)*4 + 3] ^ t3);
    }
}

void aes256_expand_key(const unsigned char key[32], unsigned char rk[240])
{
    int i;
    unsigned char t0, t1, t2, t3, tmp;

    memcpy(rk, key, 32);                         /* Nk = 8 words */
    for (i = 8; i < 60; i++) {                   /* 60 words = 15 round keys */
        t0 = rk[(i-1)*4 + 0];
        t1 = rk[(i-1)*4 + 1];
        t2 = rk[(i-1)*4 + 2];
        t3 = rk[(i-1)*4 + 3];
        if ((i & 7) == 0) {                     /* RotWord + SubWord + Rcon */
            tmp = t0;
            t0 = (unsigned char)(sbox[t1] ^ rcon[i/8]);
            t1 = sbox[t2];
            t2 = sbox[t3];
            t3 = sbox[tmp];
        } else if ((i & 7) == 4) {              /* SubWord only (AES-256 extra) */
            t0 = sbox[t0]; t1 = sbox[t1]; t2 = sbox[t2]; t3 = sbox[t3];
        }
        rk[i*4 + 0] = (unsigned char)(rk[(i-8)*4 + 0] ^ t0);
        rk[i*4 + 1] = (unsigned char)(rk[(i-8)*4 + 1] ^ t1);
        rk[i*4 + 2] = (unsigned char)(rk[(i-8)*4 + 2] ^ t2);
        rk[i*4 + 3] = (unsigned char)(rk[(i-8)*4 + 3] ^ t3);
    }
}

static void add_round_key(unsigned char *s, const unsigned char *rk)
{
    int i;
    for (i = 0; i < 16; i++)
        s[i] ^= rk[i];
}

static void sub_bytes(unsigned char *s)
{
    int i;
    for (i = 0; i < 16; i++)
        s[i] = sbox[s[i]];
}

static void inv_sub_bytes(unsigned char *s)
{
    int i;
    for (i = 0; i < 16; i++)
        s[i] = inv_sbox[s[i]];
}

static void shift_rows(unsigned char *s)
{
    unsigned char t;
    /* row 1: left rotate by 1  (indices 1,5,9,13) */
    t = s[1];  s[1] = s[5];   s[5] = s[9];   s[9] = s[13];  s[13] = t;
    /* row 2: left rotate by 2 */
    t = s[2];  s[2] = s[10];  s[10] = t;
    t = s[6];  s[6] = s[14];  s[14] = t;
    /* row 3: left rotate by 3 == right by 1 */
    t = s[15]; s[15] = s[11]; s[11] = s[7];  s[7] = s[3];   s[3] = t;
}

static void inv_shift_rows(unsigned char *s)
{
    unsigned char t;
    /* row 1: right rotate by 1 */
    t = s[13]; s[13] = s[9];  s[9] = s[5];   s[5] = s[1];   s[1] = t;
    /* row 2: rotate by 2 */
    t = s[2];  s[2] = s[10];  s[10] = t;
    t = s[6];  s[6] = s[14];  s[14] = t;
    /* row 3: right rotate by 3 == left by 1 */
    t = s[3];  s[3] = s[7];   s[7] = s[11];  s[11] = s[15]; s[15] = t;
}

static void mix_columns(unsigned char *s)
{
    int c;
    for (c = 0; c < 4; c++) {
        unsigned char *p = s + 4*c;
        unsigned char a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (unsigned char)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        p[1] = (unsigned char)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        p[2] = (unsigned char)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        p[3] = (unsigned char)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

static void inv_mix_columns(unsigned char *s)
{
    int c;
    for (c = 0; c < 4; c++) {
        unsigned char *p = s + 4*c;
        unsigned char a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = (unsigned char)(gmul(a0,14) ^ gmul(a1,11) ^ gmul(a2,13) ^ gmul(a3, 9));
        p[1] = (unsigned char)(gmul(a0, 9) ^ gmul(a1,14) ^ gmul(a2,11) ^ gmul(a3,13));
        p[2] = (unsigned char)(gmul(a0,13) ^ gmul(a1, 9) ^ gmul(a2,14) ^ gmul(a3,11));
        p[3] = (unsigned char)(gmul(a0,11) ^ gmul(a1,13) ^ gmul(a2, 9) ^ gmul(a3,14));
    }
}

/* Key-size-generic single block: nr rounds over the matching schedule rk. */
void aes_encrypt(const unsigned char *rk, int nr,
                 const unsigned char in[16], unsigned char out[16])
{
    unsigned char s[16];
    int r;
    memcpy(s, in, 16);
    add_round_key(s, rk);
    for (r = 1; r < nr; r++) {
        sub_bytes(s);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, rk + 16*r);
    }
    sub_bytes(s);
    shift_rows(s);
    add_round_key(s, rk + 16*nr);
    memcpy(out, s, 16);
}

void aes_decrypt(const unsigned char *rk, int nr,
                 const unsigned char in[16], unsigned char out[16])
{
    unsigned char s[16];
    int r;
    memcpy(s, in, 16);
    add_round_key(s, rk + 16*nr);
    for (r = nr - 1; r >= 1; r--) {
        inv_shift_rows(s);
        inv_sub_bytes(s);
        add_round_key(s, rk + 16*r);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    inv_sub_bytes(s);
    add_round_key(s, rk);
    memcpy(out, s, 16);
}

void aes128_encrypt(const unsigned char rk[176],
                    const unsigned char in[16], unsigned char out[16])
{ aes_encrypt(rk, 10, in, out); }
void aes128_decrypt(const unsigned char rk[176],
                    const unsigned char in[16], unsigned char out[16])
{ aes_decrypt(rk, 10, in, out); }
void aes256_encrypt(const unsigned char rk[240],
                    const unsigned char in[16], unsigned char out[16])
{ aes_encrypt(rk, 14, in, out); }
void aes256_decrypt(const unsigned char rk[240],
                    const unsigned char in[16], unsigned char out[16])
{ aes_decrypt(rk, 14, in, out); }

int aes_cbc_encrypt(const unsigned char *rk, int nr, const unsigned char iv[16],
                    const unsigned char *in, unsigned char *out, int len)
{
    unsigned char prev[16], blk[16];
    int i, j;
    if (len <= 0 || (len & 15))
        return -1;
    memcpy(prev, iv, 16);
    for (i = 0; i < len; i += 16) {
        for (j = 0; j < 16; j++)
            blk[j] = (unsigned char)(in[i+j] ^ prev[j]);
        aes_encrypt(rk, nr, blk, out + i);
        memcpy(prev, out + i, 16);
    }
    return 0;
}

int aes_cbc_decrypt(const unsigned char *rk, int nr, const unsigned char iv[16],
                    const unsigned char *in, unsigned char *out, int len)
{
    unsigned char prev[16], ct[16];
    int i, j;
    if (len <= 0 || (len & 15))
        return -1;
    memcpy(prev, iv, 16);
    for (i = 0; i < len; i += 16) {
        memcpy(ct, in + i, 16);             /* save (in case in == out) */
        aes_decrypt(rk, nr, in + i, out + i);
        for (j = 0; j < 16; j++)
            out[i+j] ^= prev[j];
        memcpy(prev, ct, 16);
    }
    return 0;
}

int aes128_cbc_encrypt(const unsigned char rk[176], const unsigned char iv[16],
                       const unsigned char *in, unsigned char *out, int len)
{ return aes_cbc_encrypt(rk, 10, iv, in, out, len); }
int aes128_cbc_decrypt(const unsigned char rk[176], const unsigned char iv[16],
                       const unsigned char *in, unsigned char *out, int len)
{ return aes_cbc_decrypt(rk, 10, iv, in, out, len); }
