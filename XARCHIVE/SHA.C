/* ==========================================================================
 * SHA.C - SHA-1 (FIPS-180) + SHA-256 (FIPS-180-4) + HMAC (RFC 2104).
 * Portable C89.  u32 is exactly 32 bits on the Win32 targets, so shifts
 * wrap mod 2^32 naturally and no masking is needed.
 * ========================================================================== */
#include "sha.h"
#include <string.h>

#define ROTL(x,n)  (((x) << (n)) | ((x) >> (32 - (n))))
#define ROTR(x,n)  (((x) >> (n)) | ((x) << (32 - (n))))

/* Common: fold 'len' bytes into the running 64-bit bit-count. */
static void add_bits(u32 count[2], unsigned int len)
{
    u32 add = (u32)len << 3;
    count[0] += add;
    if (count[0] < add)
        count[1]++;
    count[1] += (u32)len >> 29;
}

/* ==========================================================================
 * SHA-1
 * ========================================================================== */
static void sha1_transform(SHA1_CTX *c, const unsigned char *p)
{
    u32 w[80], a, b, cc, d, e, f, k, t;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((u32)p[4*i] << 24) | ((u32)p[4*i+1] << 16) |
               ((u32)p[4*i+2] << 8) | (u32)p[4*i+3];
    for (i = 16; i < 80; i++)
        w[i] = ROTL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4];
    for (i = 0; i < 80; i++) {
        if (i < 20)      { f = (b & cc) | ((~b) & d);           k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d;                      k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ cc ^ d;                      k = 0xCA62C1D6; }
        t = ROTL(a,5) + f + e + k + w[i];
        e = d; d = cc; cc = ROTL(b,30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

void sha1_init(SHA1_CTX *c)
{
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE;
    c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
    c->count[0] = c->count[1] = 0;
    c->nbuf = 0;
}

void sha1_update(SHA1_CTX *c, const void *data, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)data;
    add_bits(c->count, len);
    while (len > 0) {
        unsigned int n = 64 - c->nbuf;
        if (n > len) n = len;
        memcpy(c->buf + c->nbuf, p, n);
        c->nbuf += n; p += n; len -= n;
        if (c->nbuf == 64) { sha1_transform(c, c->buf); c->nbuf = 0; }
    }
}

void sha1_final(SHA1_CTX *c, unsigned char out[20])
{
    unsigned char lenb[8], pad = 0x80, zero = 0x00;
    u32 hi = c->count[1], lo = c->count[0];
    int i;

    lenb[0] = (unsigned char)(hi >> 24); lenb[1] = (unsigned char)(hi >> 16);
    lenb[2] = (unsigned char)(hi >> 8);  lenb[3] = (unsigned char)hi;
    lenb[4] = (unsigned char)(lo >> 24); lenb[5] = (unsigned char)(lo >> 16);
    lenb[6] = (unsigned char)(lo >> 8);  lenb[7] = (unsigned char)lo;

    sha1_update(c, &pad, 1);
    while (c->nbuf != 56)
        sha1_update(c, &zero, 1);
    sha1_update(c, lenb, 8);

    for (i = 0; i < 5; i++) {
        out[4*i]   = (unsigned char)(c->h[i] >> 24);
        out[4*i+1] = (unsigned char)(c->h[i] >> 16);
        out[4*i+2] = (unsigned char)(c->h[i] >> 8);
        out[4*i+3] = (unsigned char)(c->h[i]);
    }
}

void sha1(const void *data, unsigned int len, unsigned char out[20])
{
    SHA1_CTX c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* ==========================================================================
 * SHA-256
 * ========================================================================== */
static const u32 K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define CH(x,y,z)   (((x) & (y)) ^ ((~(x)) & (z)))
#define MAJ(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x)    (ROTR(x,2) ^ ROTR(x,13) ^ ROTR(x,22))
#define BSIG1(x)    (ROTR(x,6) ^ ROTR(x,11) ^ ROTR(x,25))
#define SSIG0(x)    (ROTR(x,7) ^ ROTR(x,18) ^ ((x) >> 3))
#define SSIG1(x)    (ROTR(x,17) ^ ROTR(x,19) ^ ((x) >> 10))

static void sha256_transform(SHA256_CTX *c, const unsigned char *p)
{
    u32 w[64], a, b, cc, d, e, f, g, h, t1, t2;
    int i;

    for (i = 0; i < 16; i++)
        w[i] = ((u32)p[4*i] << 24) | ((u32)p[4*i+1] << 16) |
               ((u32)p[4*i+2] << 8) | (u32)p[4*i+3];
    for (i = 16; i < 64; i++)
        w[i] = SSIG1(w[i-2]) + w[i-7] + SSIG0(w[i-15]) + w[i-16];

    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6];  h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + BSIG1(e) + CH(e,f,g) + K256[i] + w[i];
        t2 = BSIG0(a) + MAJ(a,b,cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g;  c->h[7] += h;
}

void sha256_init(SHA256_CTX *c)
{
    c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85; c->h[2] = 0x3c6ef372;
    c->h[3] = 0xa54ff53a; c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
    c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
    c->count[0] = c->count[1] = 0;
    c->nbuf = 0;
}

void sha256_update(SHA256_CTX *c, const void *data, unsigned int len)
{
    const unsigned char *p = (const unsigned char *)data;
    add_bits(c->count, len);
    while (len > 0) {
        unsigned int n = 64 - c->nbuf;
        if (n > len) n = len;
        memcpy(c->buf + c->nbuf, p, n);
        c->nbuf += n; p += n; len -= n;
        if (c->nbuf == 64) { sha256_transform(c, c->buf); c->nbuf = 0; }
    }
}

void sha256_final(SHA256_CTX *c, unsigned char out[32])
{
    unsigned char lenb[8], pad = 0x80, zero = 0x00;
    u32 hi = c->count[1], lo = c->count[0];
    int i;

    lenb[0] = (unsigned char)(hi >> 24); lenb[1] = (unsigned char)(hi >> 16);
    lenb[2] = (unsigned char)(hi >> 8);  lenb[3] = (unsigned char)hi;
    lenb[4] = (unsigned char)(lo >> 24); lenb[5] = (unsigned char)(lo >> 16);
    lenb[6] = (unsigned char)(lo >> 8);  lenb[7] = (unsigned char)lo;

    sha256_update(c, &pad, 1);
    while (c->nbuf != 56)
        sha256_update(c, &zero, 1);
    sha256_update(c, lenb, 8);

    for (i = 0; i < 8; i++) {
        out[4*i]   = (unsigned char)(c->h[i] >> 24);
        out[4*i+1] = (unsigned char)(c->h[i] >> 16);
        out[4*i+2] = (unsigned char)(c->h[i] >> 8);
        out[4*i+3] = (unsigned char)(c->h[i]);
    }
}

void sha256(const void *data, unsigned int len, unsigned char out[32])
{
    SHA256_CTX c;
    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, out);
}

/* ==========================================================================
 * HMAC (RFC 2104)
 * ========================================================================== */
void hmac_sha1(const unsigned char *key, int keylen,
               const unsigned char *msg, int msglen, unsigned char out[20])
{
    unsigned char k[64], ipad[64], opad[64], inner[20];
    SHA1_CTX c;
    int i;

    memset(k, 0, 64);
    if (keylen > 64)
        sha1(key, (unsigned int)keylen, k);     /* long key -> hashed */
    else
        memcpy(k, key, keylen);

    for (i = 0; i < 64; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, msg, (unsigned int)msglen);
    sha1_final(&c, inner);

    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

void hmac_sha256(const unsigned char *key, int keylen,
                 const unsigned char *msg, int msglen, unsigned char out[32])
{
    unsigned char k[64], ipad[64], opad[64], inner[32];
    SHA256_CTX c;
    int i;

    memset(k, 0, 64);
    if (keylen > 64)
        sha256(key, (unsigned int)keylen, k);
    else
        memcpy(k, key, keylen);

    for (i = 0; i < 64; i++) {
        ipad[i] = (unsigned char)(k[i] ^ 0x36);
        opad[i] = (unsigned char)(k[i] ^ 0x5c);
    }

    sha256_init(&c);
    sha256_update(&c, ipad, 64);
    sha256_update(&c, msg, (unsigned int)msglen);
    sha256_final(&c, inner);

    sha256_init(&c);
    sha256_update(&c, opad, 64);
    sha256_update(&c, inner, 32);
    sha256_final(&c, out);
}
