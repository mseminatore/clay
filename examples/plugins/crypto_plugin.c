/*
 * crypto_plugin.c — Crypto plugin for Clay language
 *
 * Provides SHA-256, SHA-1, MD5 hashing, HMAC-SHA256, Base64 encode/decode,
 * and cryptographic random bytes. All implementations are pure C with no
 * external dependencies.
 *
 * Usage in Clay:
 *   Crypto = System plugin("crypto")
 *   Crypto sha256("hello")           // "2cf24dba5fb0a30e..."
 *   Crypto md5("hello")              // "5d41402abc4b2a76..."
 *   Crypto base64Encode("hello")     // "aGVsbG8="
 *   Crypto base64Decode("aGVsbG8=")  // "hello"
 *   Crypto hmacSha256("key", "data") // hex string
 *   Crypto randomBytes(16)           // 32-char hex string
 */
#include "clay_plugin.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const ClayPluginAPI *api;

/* ================================================================== */
/*  Utility                                                            */
/* ================================================================== */

static void bytes_to_hex(const unsigned char *bytes, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

/* ================================================================== */
/*  SHA-256 (FIPS 180-4)                                               */
/* ================================================================== */

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define SHA256_ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHA256_CH(x,y,z)  (((x) & (y)) ^ (~(x) & (z)))
#define SHA256_MAJ(x,y,z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define SHA256_EP0(x)  (SHA256_ROTR(x, 2)  ^ SHA256_ROTR(x, 13) ^ SHA256_ROTR(x, 22))
#define SHA256_EP1(x)  (SHA256_ROTR(x, 6)  ^ SHA256_ROTR(x, 11) ^ SHA256_ROTR(x, 25))
#define SHA256_SIG0(x) (SHA256_ROTR(x, 7)  ^ SHA256_ROTR(x, 18) ^ ((x) >> 3))
#define SHA256_SIG1(x) (SHA256_ROTR(x, 17) ^ SHA256_ROTR(x, 19) ^ ((x) >> 10))

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t  data[64];
    uint32_t datalen;
} SHA256_CTX;

static void sha256_init(SHA256_CTX *ctx) {
    ctx->datalen = 0;
    ctx->bitlen  = 0;
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
}

static void sha256_transform(SHA256_CTX *ctx) {
    uint32_t w[64], a, b, c, d, e, f, g, h, t1, t2;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)ctx->data[i*4] << 24) | ((uint32_t)ctx->data[i*4+1] << 16) |
               ((uint32_t)ctx->data[i*4+2] << 8)  | (uint32_t)ctx->data[i*4+3];
    for (int i = 16; i < 64; i++)
        w[i] = SHA256_SIG1(w[i-2]) + w[i-7] + SHA256_SIG0(w[i-15]) + w[i-16];
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (int i = 0; i < 64; i++) {
        t1 = h + SHA256_EP1(e) + SHA256_CH(e,f,g) + sha256_k[i] + w[i];
        t2 = SHA256_EP0(a) + SHA256_MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(SHA256_CTX *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0;
        sha256_transform(ctx);
        i = 0;
    }
    while (i < 56) ctx->data[i++] = 0;
    ctx->bitlen += (uint64_t)ctx->datalen * 8;
    for (int j = 7; j >= 0; j--)
        ctx->data[56 + (7 - j)] = (uint8_t)(ctx->bitlen >> (j * 8));
    sha256_transform(ctx);
    for (int j = 0; j < 8; j++) {
        hash[j*4]   = (uint8_t)(ctx->state[j] >> 24);
        hash[j*4+1] = (uint8_t)(ctx->state[j] >> 16);
        hash[j*4+2] = (uint8_t)(ctx->state[j] >> 8);
        hash[j*4+3] = (uint8_t)(ctx->state[j]);
    }
}

static void compute_sha256(const char *input, size_t len, uint8_t hash[32]) {
    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)input, len);
    sha256_final(&ctx, hash);
}

/* ================================================================== */
/*  SHA-1 (RFC 3174)                                                   */
/* ================================================================== */

#define SHA1_ROTL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

typedef struct {
    uint32_t state[5];
    uint64_t count;
    uint8_t  buffer[64];
} SHA1_CTX;

static void sha1_init(SHA1_CTX *ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x67452301; ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE; ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
}

static void sha1_transform(SHA1_CTX *ctx, const uint8_t block[64]) {
    uint32_t w[80], a, b, c, d, e, temp;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8)  | (uint32_t)block[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = SHA1_ROTL(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2];
    d = ctx->state[3]; e = ctx->state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if      (i < 20) { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
        temp = SHA1_ROTL(a, 5) + f + e + k + w[i];
        e = d; d = c; c = SHA1_ROTL(b, 30); b = a; a = temp;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e;
}

static void sha1_update(SHA1_CTX *ctx, const uint8_t *data, size_t len) {
    size_t idx = (size_t)(ctx->count & 63);
    ctx->count += len;
    for (size_t i = 0; i < len; i++) {
        ctx->buffer[idx++] = data[i];
        if (idx == 64) {
            sha1_transform(ctx, ctx->buffer);
            idx = 0;
        }
    }
}

static void sha1_final(SHA1_CTX *ctx, uint8_t hash[20]) {
    uint64_t bits = ctx->count * 8;
    size_t idx = (size_t)(ctx->count & 63);
    ctx->buffer[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buffer[idx++] = 0;
        sha1_transform(ctx, ctx->buffer);
        idx = 0;
    }
    while (idx < 56) ctx->buffer[idx++] = 0;
    for (int i = 7; i >= 0; i--)
        ctx->buffer[56 + (7 - i)] = (uint8_t)(bits >> (i * 8));
    sha1_transform(ctx, ctx->buffer);
    for (int i = 0; i < 5; i++) {
        hash[i*4]   = (uint8_t)(ctx->state[i] >> 24);
        hash[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        hash[i*4+2] = (uint8_t)(ctx->state[i] >> 8);
        hash[i*4+3] = (uint8_t)(ctx->state[i]);
    }
}

static void compute_sha1(const char *input, size_t len, uint8_t hash[20]) {
    SHA1_CTX ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, (const uint8_t *)input, len);
    sha1_final(&ctx, hash);
}

/* ================================================================== */
/*  MD5 (RFC 1321)                                                     */
/* ================================================================== */

#define MD5_F(x,y,z) (((x) & (y)) | (~(x) & (z)))
#define MD5_G(x,y,z) (((x) & (z)) | ((y) & ~(z)))
#define MD5_H(x,y,z) ((x) ^ (y) ^ (z))
#define MD5_I(x,y,z) ((y) ^ ((x) | ~(z)))
#define MD5_ROTL(x,n) (((x) << (n)) | ((x) >> (32 - (n))))

static const uint32_t md5_t[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const int md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static void compute_md5(const char *input, size_t len, uint8_t hash[16]) {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    /* Pad message: append 0x80, zeros, then 64-bit length */
    size_t padded_len = ((len + 8) / 64 + 1) * 64;
    uint8_t *msg = (uint8_t *)calloc(padded_len, 1);
    memcpy(msg, input, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    memcpy(msg + padded_len - 8, &bits, 8); /* little-endian */

    /* Process each 64-byte block */
    for (size_t offset = 0; offset < padded_len; offset += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; i++)
            memcpy(&m[i], msg + offset + i * 4, 4); /* little-endian */

        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; i++) {
            uint32_t f, g_idx;
            if      (i < 16) { f = MD5_F(b,c,d); g_idx = (uint32_t)i; }
            else if (i < 32) { f = MD5_G(b,c,d); g_idx = (uint32_t)(5*i + 1) % 16; }
            else if (i < 48) { f = MD5_H(b,c,d); g_idx = (uint32_t)(3*i + 5) % 16; }
            else              { f = MD5_I(b,c,d); g_idx = (uint32_t)(7*i) % 16; }
            uint32_t temp = d;
            d = c; c = b;
            b = b + MD5_ROTL(a + f + md5_t[i] + m[g_idx], md5_s[i]);
            a = temp;
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    free(msg);

    /* Output in little-endian */
    memcpy(hash,      &a0, 4);
    memcpy(hash + 4,  &b0, 4);
    memcpy(hash + 8,  &c0, 4);
    memcpy(hash + 12, &d0, 4);
}

/* ================================================================== */
/*  HMAC-SHA256 (RFC 2104)                                             */
/* ================================================================== */

static void compute_hmac_sha256(const char *key, size_t key_len,
                                const char *data, size_t data_len,
                                uint8_t hash[32]) {
    uint8_t k[64];
    memset(k, 0, 64);

    if (key_len > 64) {
        compute_sha256(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }

    /* Inner: SHA256(k ^ ipad || data) */
    uint8_t inner_key[64];
    for (int i = 0; i < 64; i++) inner_key[i] = k[i] ^ 0x36;

    SHA256_CTX ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, inner_key, 64);
    sha256_update(&ctx, (const uint8_t *)data, data_len);
    uint8_t inner_hash[32];
    sha256_final(&ctx, inner_hash);

    /* Outer: SHA256(k ^ opad || inner_hash) */
    uint8_t outer_key[64];
    for (int i = 0; i < 64; i++) outer_key[i] = k[i] ^ 0x5c;

    sha256_init(&ctx);
    sha256_update(&ctx, outer_key, 64);
    sha256_update(&ctx, inner_hash, 32);
    sha256_final(&ctx, hash);
}

/* ================================================================== */
/*  Base64 (RFC 4648)                                                  */
/* ================================================================== */

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const uint8_t *data, size_t len) {
    size_t out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= (uint32_t)data[i + 2];
        out[j++] = b64_chars[(n >> 18) & 63];
        out[j++] = b64_chars[(n >> 12) & 63];
        out[j++] = (i + 1 < len) ? b64_chars[(n >> 6) & 63] : '=';
        out[j++] = (i + 2 < len) ? b64_chars[n & 63] : '=';
    }
    out[j] = '\0';
    return out;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t *base64_decode(const char *input, size_t *out_len) {
    size_t in_len = strlen(input);
    if (in_len % 4 != 0) { *out_len = 0; return NULL; }
    size_t alloc = in_len / 4 * 3;
    if (in_len > 0 && input[in_len - 1] == '=') alloc--;
    if (in_len > 1 && input[in_len - 2] == '=') alloc--;
    uint8_t *out = (uint8_t *)malloc(alloc + 1);
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int a = b64_decode_char(input[i]);
        int b = b64_decode_char(input[i + 1]);
        int c = (input[i + 2] != '=') ? b64_decode_char(input[i + 2]) : 0;
        int d = (input[i + 3] != '=') ? b64_decode_char(input[i + 3]) : 0;
        if (a < 0 || b < 0) break;
        uint32_t n = ((uint32_t)a << 18) | ((uint32_t)b << 12) | ((uint32_t)c << 6) | (uint32_t)d;
        out[j++] = (uint8_t)(n >> 16);
        if (input[i + 2] != '=') out[j++] = (uint8_t)(n >> 8);
        if (input[i + 3] != '=') out[j++] = (uint8_t)n;
    }
    out[j] = '\0';
    *out_len = j;
    return out;
}

/* ================================================================== */
/*  Random bytes                                                       */
/* ================================================================== */

static void random_bytes(uint8_t *buf, size_t len) {
#ifdef _WIN32
    /* Use BCryptGenRandom on Windows */
    typedef long NTSTATUS;
    typedef void *BCRYPT_ALG_HANDLE;
    __declspec(dllimport) NTSTATUS __stdcall BCryptGenRandom(
        BCRYPT_ALG_HANDLE hAlg, unsigned char *pbBuffer,
        unsigned long cbBuffer, unsigned long dwFlags);
    #define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
    BCryptGenRandom(NULL, buf, (unsigned long)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    /* Use /dev/urandom on Unix */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        fread(buf, 1, len, f);
        fclose(f);
    } else {
        /* Fallback: use rand() (not cryptographically secure) */
        for (size_t i = 0; i < len; i++)
            buf[i] = (uint8_t)(rand() & 0xff);
    }
#endif
}

/* ================================================================== */
/*  Plugin methods                                                     */
/* ================================================================== */

static ClayPluginValue crypto_sha256(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("sha256 requires 1 argument", "ArgumentError"); return 0; }
    const char *input = api->to_string(argv[0]);
    uint8_t hash[32];
    compute_sha256(input, strlen(input), hash);
    char hex[65];
    bytes_to_hex(hash, 32, hex);
    return api->make_string(hex);
}

static ClayPluginValue crypto_sha1(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("sha1 requires 1 argument", "ArgumentError"); return 0; }
    const char *input = api->to_string(argv[0]);
    uint8_t hash[20];
    compute_sha1(input, strlen(input), hash);
    char hex[41];
    bytes_to_hex(hash, 20, hex);
    return api->make_string(hex);
}

static ClayPluginValue crypto_md5(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("md5 requires 1 argument", "ArgumentError"); return 0; }
    const char *input = api->to_string(argv[0]);
    uint8_t hash[16];
    compute_md5(input, strlen(input), hash);
    char hex[33];
    bytes_to_hex(hash, 16, hex);
    return api->make_string(hex);
}

static ClayPluginValue crypto_hmac_sha256(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 2) { api->raise("hmacSha256 requires 2 arguments (key, data)", "ArgumentError"); return 0; }
    const char *key  = api->to_string(argv[0]);
    const char *data = api->to_string(argv[1]);
    uint8_t hash[32];
    compute_hmac_sha256(key, strlen(key), data, strlen(data), hash);
    char hex[65];
    bytes_to_hex(hash, 32, hex);
    return api->make_string(hex);
}

static ClayPluginValue crypto_base64_encode(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("base64Encode requires 1 argument", "ArgumentError"); return 0; }
    const char *input = api->to_string(argv[0]);
    char *encoded = base64_encode((const uint8_t *)input, strlen(input));
    ClayPluginValue result = api->make_string(encoded);
    free(encoded);
    return result;
}

static ClayPluginValue crypto_base64_decode(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("base64Decode requires 1 argument", "ArgumentError"); return 0; }
    const char *input = api->to_string(argv[0]);
    size_t out_len;
    uint8_t *decoded = base64_decode(input, &out_len);
    if (!decoded) {
        api->raise("Invalid base64 input", "CryptoError");
        return 0;
    }
    ClayPluginValue result = api->make_string((const char *)decoded);
    free(decoded);
    return result;
}

static ClayPluginValue crypto_random_bytes(ClayPluginObject self, int argc, ClayPluginValue *argv) {
    (void)self;
    if (argc < 1) { api->raise("randomBytes requires 1 argument (byte count)", "ArgumentError"); return 0; }
    intptr_t n = api->to_int(argv[0]);
    if (n <= 0 || n > 1024) { api->raise("randomBytes: count must be 1-1024", "ArgumentError"); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    random_bytes(buf, (size_t)n);
    char *hex = (char *)malloc((size_t)(n * 2 + 1));
    bytes_to_hex(buf, (size_t)n, hex);
    ClayPluginValue result = api->make_string(hex);
    free(buf);
    free(hex);
    return result;
}

/* ================================================================== */
/*  Plugin registration                                                */
/* ================================================================== */

static ClayPluginMethod methods[] = {
    { "sha256",       crypto_sha256 },
    { "sha1",         crypto_sha1 },
    { "md5",          crypto_md5 },
    { "hmacSha256",   crypto_hmac_sha256 },
    { "base64Encode", crypto_base64_encode },
    { "base64Decode", crypto_base64_decode },
    { "randomBytes",  crypto_random_bytes },
    { NULL, NULL }
};

CLAY_PLUGIN_EXPORT void clay_plugin_init(const ClayPluginAPI *clay_api) {
    api = clay_api;
    ClayPluginType def = { "Crypto", methods, NULL, 0 };
    api->register_type(&def);
}
