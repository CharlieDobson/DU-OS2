/* ==========================================================================
 * SHA.H - SHA-1, SHA-256 and HMAC for NETSPLOR's TLS 1.2 crypto stack.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89, no OS dependency).
 * ==========================================================================
 * SHA-1  : record MAC (HMAC-SHA1) for TLS_RSA_WITH_AES_128_CBC_SHA, and the
 *          handshake hash where TLS 1.0/1.1 style is needed.
 * SHA-256: the TLS 1.2 PRF (P_SHA256) and the Finished handshake hash.
 * HMAC   : keyed MAC over either hash.
 * ========================================================================== */
#ifndef NETSPLOR_SHA_H
#define NETSPLOR_SHA_H

#define SHA1_DIGEST_LEN     20
#define SHA256_DIGEST_LEN   32
#define SHA_BLOCK_LEN       64      /* both use a 64-byte block */

#ifndef NST_U32_DEFINED
#define NST_U32_DEFINED
typedef unsigned int u32;           /* 32-bit on all Win32 targets */
#endif

typedef struct {
    u32           h[5];
    u32           count[2];         /* 64-bit message length in bits */
    unsigned char buf[64];
    unsigned int  nbuf;
} SHA1_CTX;

typedef struct {
    u32           h[8];
    u32           count[2];
    unsigned char buf[64];
    unsigned int  nbuf;
} SHA256_CTX;

/* --- SHA-1 --- */
void sha1_init(SHA1_CTX *c);
void sha1_update(SHA1_CTX *c, const void *data, unsigned int len);
void sha1_final(SHA1_CTX *c, unsigned char out[20]);
void sha1(const void *data, unsigned int len, unsigned char out[20]);

/* --- SHA-256 --- */
void sha256_init(SHA256_CTX *c);
void sha256_update(SHA256_CTX *c, const void *data, unsigned int len);
void sha256_final(SHA256_CTX *c, unsigned char out[32]);
void sha256(const void *data, unsigned int len, unsigned char out[32]);

/* --- HMAC (one-shot) --- */
void hmac_sha1(const unsigned char *key, int keylen,
               const unsigned char *msg, int msglen, unsigned char out[20]);
void hmac_sha256(const unsigned char *key, int keylen,
                 const unsigned char *msg, int msglen, unsigned char out[32]);

#endif /* NETSPLOR_SHA_H */
