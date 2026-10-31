/*
 * kernel_crypto.c - Xbox Crypto Functions
 *
 * Implements SHA-1, RC4, HMAC-SHA1, and public key operations.
 *
 * The Xbox uses these for:
 *   - Save game signing (SHA-1 + HMAC)
 *   - Xbox Live authentication (RSA, RC4)
 *   - Content verification (SHA-1)
 *
 * Implementation uses Windows BCrypt API (available since Vista).
 * For save game compatibility, all crypto must produce identical outputs
 * to the original Xbox implementations.
 *
 * Note: RC4 is implemented manually since BCrypt deprecated it in newer
 * Windows versions. The Xbox RC4 is standard RC4 (ARC4).
 */

#include "kernel.h"
#include <string.h>

/* SHA-1 / RC4 / HMAC below are portable software implementations, so the
 * only platform dependency is BCrypt's types in the (unused) SHA_INTERNAL
 * struct. Keep it Windows-only. */
#if defined(_WIN32)
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

/* ============================================================================
 * SHA-1
 *
 * Xbox SHA-1 is standard FIPS 180-1 SHA-1. We use BCrypt for correctness.
 * The XBOX_SHA_CONTEXT is our own structure (defined in kernel.h).
 * We store the BCrypt hash handle inside it.
 * ============================================================================ */

/* We use a wrapper since XBOX_SHA_CONTEXT may not be large enough for
 * BCrypt's internal state. Store BCrypt handles in a separate mapping. */

#if defined(_WIN32)
typedef struct _SHA_INTERNAL {
    BCRYPT_ALG_HANDLE  hAlg;
    BCRYPT_HASH_HANDLE hHash;
    UCHAR              hashObject[256]; /* BCrypt hash object buffer */
} SHA_INTERNAL;
#endif

/*
 * Since XBOX_SHA_CONTEXT is only 92 bytes (State[5] + Count[2] + Buffer[64]),
 * we can't fit a BCrypt handle in it. We use a simple software SHA-1 instead.
 * This also ensures byte-exact compatibility with the Xbox implementation.
 */

/* Software SHA-1 implementation - matches Xbox/FIPS exactly */

static ULONG sha1_rotate_left(ULONG value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_transform(ULONG state[5], const UCHAR buffer[64])
{
    ULONG a, b, c, d, e, f, k, temp;
    ULONG w[80];
    int i;

    /* Prepare message schedule */
    for (i = 0; i < 16; i++) {
        w[i] = ((ULONG)buffer[i * 4] << 24)
             | ((ULONG)buffer[i * 4 + 1] << 16)
             | ((ULONG)buffer[i * 4 + 2] << 8)
             | ((ULONG)buffer[i * 4 + 3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1_rotate_left(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }

        temp = sha1_rotate_left(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotate_left(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

VOID __stdcall xbox_XcSHAInit(PXBOX_SHA_CONTEXT ShaContext)
{
    if (!ShaContext)
        return;

    ShaContext->State[0] = 0x67452301;
    ShaContext->State[1] = 0xEFCDAB89;
    ShaContext->State[2] = 0x98BADCFE;
    ShaContext->State[3] = 0x10325476;
    ShaContext->State[4] = 0xC3D2E1F0;
    ShaContext->Count[0] = 0;
    ShaContext->Count[1] = 0;
    memset(ShaContext->Buffer, 0, 64);
}

VOID __stdcall xbox_XcSHAUpdate(PXBOX_SHA_CONTEXT ShaContext, const UCHAR* Input, ULONG InputLength)
{
    ULONG i, index, part_len;

    if (!ShaContext || !Input || InputLength == 0)
        return;

    index = (ShaContext->Count[0] >> 3) & 0x3F;

    /* Update bit count */
    ShaContext->Count[0] += (InputLength << 3);
    if (ShaContext->Count[0] < (InputLength << 3))
        ShaContext->Count[1]++;
    ShaContext->Count[1] += (InputLength >> 29);

    part_len = 64 - index;

    if (InputLength >= part_len) {
        memcpy(&ShaContext->Buffer[index], Input, part_len);
        sha1_transform(ShaContext->State, ShaContext->Buffer);

        for (i = part_len; i + 63 < InputLength; i += 64) {
            sha1_transform(ShaContext->State, &Input[i]);
        }

        index = 0;
    } else {
        i = 0;
    }

    memcpy(&ShaContext->Buffer[index], &Input[i], InputLength - i);
}

VOID __stdcall xbox_XcSHAFinal(PXBOX_SHA_CONTEXT ShaContext, UCHAR* Digest)
{
    UCHAR finalcount[8];
    UCHAR pad;
    ULONG i;

    if (!ShaContext || !Digest)
        return;

    /* Store bit count (big-endian) */
    for (i = 0; i < 4; i++) {
        finalcount[i]     = (UCHAR)((ShaContext->Count[1] >> ((3 - i) * 8)) & 0xFF);
        finalcount[i + 4] = (UCHAR)((ShaContext->Count[0] >> ((3 - i) * 8)) & 0xFF);
    }

    /* Pad to 56 mod 64 */
    pad = 0x80;
    xbox_XcSHAUpdate(ShaContext, &pad, 1);
    pad = 0x00;
    while ((ShaContext->Count[0] >> 3) % 64 != 56) {
        xbox_XcSHAUpdate(ShaContext, &pad, 1);
    }

    /* Append length */
    xbox_XcSHAUpdate(ShaContext, finalcount, 8);

    /* Output digest (big-endian) */
    for (i = 0; i < 5; i++) {
        Digest[i * 4]     = (UCHAR)((ShaContext->State[i] >> 24) & 0xFF);
        Digest[i * 4 + 1] = (UCHAR)((ShaContext->State[i] >> 16) & 0xFF);
        Digest[i * 4 + 2] = (UCHAR)((ShaContext->State[i] >> 8) & 0xFF);
        Digest[i * 4 + 3] = (UCHAR)((ShaContext->State[i]) & 0xFF);
    }
}

/* ============================================================================
 * RC4 (ARC4)
 *
 * Standard RC4 stream cipher. Used for Xbox Live encryption and some
 * save game protection. Implemented in software for portability.
 * ============================================================================ */

VOID __stdcall xbox_XcRC4Key(PXBOX_RC4_CONTEXT Rc4Context, ULONG KeyLength, const UCHAR* Key)
{
    UCHAR temp;
    ULONG i;
    UCHAR j = 0;

    if (!Rc4Context || !Key || KeyLength == 0)
        return;

    /* KSA (Key-Scheduling Algorithm) */
    for (i = 0; i < 256; i++)
        Rc4Context->S[i] = (UCHAR)i;

    for (i = 0; i < 256; i++) {
        j = j + Rc4Context->S[i] + Key[i % KeyLength];
        temp = Rc4Context->S[i];
        Rc4Context->S[i] = Rc4Context->S[j];
        Rc4Context->S[j] = temp;
    }

    Rc4Context->i = 0;
    Rc4Context->j = 0;
}

VOID __stdcall xbox_XcRC4Crypt(PXBOX_RC4_CONTEXT Rc4Context, ULONG Length, UCHAR* Data)
{
    UCHAR temp;
    ULONG n;

    if (!Rc4Context || !Data || Length == 0)
        return;

    /* PRGA (Pseudo-Random Generation Algorithm) */
    for (n = 0; n < Length; n++) {
        Rc4Context->i++;
        Rc4Context->j += Rc4Context->S[Rc4Context->i];

        temp = Rc4Context->S[Rc4Context->i];
        Rc4Context->S[Rc4Context->i] = Rc4Context->S[Rc4Context->j];
        Rc4Context->S[Rc4Context->j] = temp;

        Data[n] ^= Rc4Context->S[(UCHAR)(Rc4Context->S[Rc4Context->i] + Rc4Context->S[Rc4Context->j])];
    }
}

/* ============================================================================
 * HMAC-SHA1
 *
 * Standard RFC 2104 HMAC using SHA-1. Used for save game authentication.
 * Xbox XcHMAC takes two data buffers to hash (Data1 and Data2, concatenated).
 * ============================================================================ */

VOID __stdcall xbox_XcHMAC(
    const UCHAR* Key, ULONG KeyLength,
    const UCHAR* Data1, ULONG Data1Length,
    const UCHAR* Data2, ULONG Data2Length,
    UCHAR* Digest)
{
    XBOX_SHA_CONTEXT sha;
    UCHAR key_block[64];
    UCHAR ipad[64];
    UCHAR opad[64];
    UCHAR inner_digest[20];
    ULONG i;

    if (!Key || !Digest)
        return;

    /* If key is longer than block size, hash it first */
    memset(key_block, 0, 64);
    if (KeyLength > 64) {
        xbox_XcSHAInit(&sha);
        xbox_XcSHAUpdate(&sha, Key, KeyLength);
        xbox_XcSHAFinal(&sha, key_block);
    } else {
        memcpy(key_block, Key, KeyLength);
    }

    /* Inner padding */
    for (i = 0; i < 64; i++)
        ipad[i] = key_block[i] ^ 0x36;

    /* Outer padding */
    for (i = 0; i < 64; i++)
        opad[i] = key_block[i] ^ 0x5C;

    /* Inner hash: SHA1(ipad || data1 || data2) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, ipad, 64);
    if (Data1 && Data1Length > 0)
        xbox_XcSHAUpdate(&sha, Data1, Data1Length);
    if (Data2 && Data2Length > 0)
        xbox_XcSHAUpdate(&sha, Data2, Data2Length);
    xbox_XcSHAFinal(&sha, inner_digest);

    /* Outer hash: SHA1(opad || inner_digest) */
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, opad, 64);
    xbox_XcSHAUpdate(&sha, inner_digest, 20);
    xbox_XcSHAFinal(&sha, Digest);

    /* Clean up sensitive data */
    SecureZeroMemory(key_block, sizeof(key_block));
    SecureZeroMemory(ipad, sizeof(ipad));
    SecureZeroMemory(opad, sizeof(opad));
    SecureZeroMemory(inner_digest, sizeof(inner_digest));
}

/* ============================================================================
 * Public Key Operations
 *
 * Used for Xbox Live authentication and content signing verification.
 * These are not needed for PC operation (no Xbox Live), so they're
 * stubbed to return failure/zero.
 * ============================================================================ */

ULONG __stdcall xbox_XcPKGetKeyLen(PVOID PublicKey)
{
    (void)PublicKey;
    return 0;
}

ULONG __stdcall xbox_XcPKDecPrivate(PVOID PrivateKey, PVOID Input, PVOID Output)
{
    (void)PrivateKey;
    (void)Input;
    (void)Output;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO, "XcPKDecPrivate: stubbed (no Xbox Live)");
    return 0; /* Failure */
}

ULONG __stdcall xbox_XcPKEncPublic(PVOID PublicKey, PVOID Input, PVOID Output)
{
    (void)PublicKey;
    (void)Input;
    (void)Output;
    xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO, "XcPKEncPublic: stubbed (no Xbox Live)");
    return 0;
}

BOOLEAN __stdcall xbox_XcVerifyPKCS1Signature(PVOID Hash, PVOID PublicKey, PVOID Signature)
{
    (void)Hash;
    (void)PublicKey;
    (void)Signature;
    /* Return TRUE to bypass signature checks */
    return TRUE;
}

/* Result = Base ^ Exponent mod Modulus, all ModulusLength 32-bit words,
 * least significant word first.
 *
 * XNet's System Link key exchange is Diffie-Hellman through this call: each
 * side raises the group generator to a random secret and later raises the
 * other's public value to the same secret. A stub that writes nothing leaves
 * both sides with garbage, XNet drops the exchange, and a joiner never sends
 * its first packet ("This game session is no longer available").
 *
 * Montgomery multiplication; the moduli used (the Oakley groups) are odd.
 * Returns TRUE as the kernel does. */
#define MODEXP_MAX_WORDS 64

static void mont_mul(ULONG *r, const ULONG *a, const ULONG *b,
                     const ULONG *m, ULONG minv, ULONG n)
{
    ULONG t[MODEXP_MAX_WORDS + 2];
    ULONG i, j;

    memset(t, 0, sizeof(ULONG) * (n + 2));
    for (i = 0; i < n; i++) {
        unsigned long long c = 0;
        ULONG u;
        for (j = 0; j < n; j++) {
            c += (unsigned long long)t[j] + (unsigned long long)a[j] * b[i];
            t[j] = (ULONG)c;
            c >>= 32;
        }
        c += t[n];
        t[n] = (ULONG)c;
        t[n + 1] = (ULONG)(c >> 32);

        u = t[0] * minv;
        c = (unsigned long long)t[0] + (unsigned long long)u * m[0];
        c >>= 32;
        for (j = 1; j < n; j++) {
            c += (unsigned long long)t[j] + (unsigned long long)u * m[j];
            t[j - 1] = (ULONG)c;
            c >>= 32;
        }
        c += t[n];
        t[n - 1] = (ULONG)c;
        t[n] = t[n + 1] + (ULONG)(c >> 32);
    }
    {   /* t < 2m: subtract m once if t >= m */
        int ge = t[n] != 0;
        if (!ge) {
            ge = 1;
            for (j = n; j-- > 0;)
                if (t[j] != m[j]) { ge = t[j] > m[j]; break; }
        }
        if (ge) {
            long long borrow = 0;
            for (j = 0; j < n; j++) {
                long long d = (long long)t[j] - m[j] + borrow;
                t[j] = (ULONG)d;
                borrow = d >> 32;
            }
        }
    }
    memcpy(r, t, sizeof(ULONG) * n);
}

/* x mod m by shift-and-subtract; used once per call to bring R^2 and the
 * base into range. */
static void mod_reduce(ULONG *r, const ULONG *x, ULONG xn, const ULONG *m, ULONG n)
{
    ULONG acc[MODEXP_MAX_WORDS + 1];
    long i;
    ULONG j;

    memset(acc, 0, sizeof(ULONG) * (n + 1));
    for (i = (long)xn * 32 - 1; i >= 0; i--) {
        ULONG carry = (x[i / 32] >> (i % 32)) & 1;
        int ge;
        for (j = 0; j <= n; j++) {
            ULONG nc = acc[j] >> 31;
            acc[j] = (acc[j] << 1) | carry;
            carry = nc;
        }
        ge = acc[n] != 0;
        if (!ge) {
            ge = 1;
            for (j = n; j-- > 0;)
                if (acc[j] != m[j]) { ge = acc[j] > m[j]; break; }
        }
        if (ge) {
            long long borrow = 0;
            for (j = 0; j < n; j++) {
                long long d = (long long)acc[j] - m[j] + borrow;
                acc[j] = (ULONG)d;
                borrow = d >> 32;
            }
            acc[n] = (ULONG)((long long)acc[n] + borrow);
        }
    }
    memcpy(r, acc, sizeof(ULONG) * n);
}

ULONG __stdcall xbox_XcModExp(PULONG Result, PULONG Base, PULONG Exponent, PULONG Modulus, ULONG ModulusLength)
{
    ULONG n = ModulusLength, minv, x, i;
    ULONG r2[2 * MODEXP_MAX_WORDS + 1], rr[MODEXP_MAX_WORDS], b[MODEXP_MAX_WORDS];
    ULONG acc[MODEXP_MAX_WORDS], one[MODEXP_MAX_WORDS];
    long bit;

    if (n == 0 || n > MODEXP_MAX_WORDS || !(Modulus[0] & 1)) {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_CRYPTO, "XcModExp: unsupported modulus (%u words)", n);
        return 0;
    }
    /* -m^-1 mod 2^32 by Newton iteration */
    x = Modulus[0];
    for (i = 0; i < 5; i++) x *= 2 - Modulus[0] * x;
    minv = (ULONG)(0u - x);

    memset(r2, 0, sizeof(ULONG) * (2 * n + 1));
    r2[2 * n] = 1;                                   /* R^2 = 2^(64n) */
    mod_reduce(rr, r2, 2 * n + 1, Modulus, n);
    mod_reduce(b, Base, n, Modulus, n);
    mont_mul(b, b, rr, Modulus, minv, n);            /* base, Montgomery form */
    memset(one, 0, sizeof(ULONG) * n);
    one[0] = 1;
    mont_mul(acc, one, rr, Modulus, minv, n);        /* 1, Montgomery form */

    for (bit = (long)n * 32 - 1; bit >= 0; bit--) {
        mont_mul(acc, acc, acc, Modulus, minv, n);
        if ((Exponent[bit / 32] >> (bit % 32)) & 1)
            mont_mul(acc, acc, b, Modulus, minv, n);
    }
    mont_mul(Result, acc, one, Modulus, minv, n);    /* back to normal form */
    return 1;
}

/* ============================================================================
 * DES / Triple DES
 *
 * XNet encrypts System Link traffic with these once the key exchange is done.
 * The key table is the caller's buffer (128 bytes for DES and 384 for triple
 * DES on the console); this keeps the raw key in it and derives the subkeys
 * per call, which is cheap at packet rates and fits any table size.
 * CipherSelect 0 = DES, 1 = triple DES (EDE). Operation 1 = encrypt,
 * 0 = decrypt.
 * ============================================================================ */

static const unsigned char DES_IP[64] = {
    58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,
    57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7 };
static const unsigned char DES_FP[64] = {
    40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,
    36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25 };
static const unsigned char DES_E[48] = {
    32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,
    16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1 };
static const unsigned char DES_P[32] = {
    16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25 };
static const unsigned char DES_PC1[56] = {
    57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,
    63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4 };
static const unsigned char DES_PC2[48] = {
    14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,
    41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32 };
static const unsigned char DES_SHIFTS[16] = { 1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1 };
static const unsigned char DES_S[8][64] = {
 {14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,
  4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
 {15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,
  0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
 {10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,
  13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
 {7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,
  10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
 {2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,
  4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
 {12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,
  9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
 {4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,
  1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
 {13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,
  7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11} };

static unsigned long long des_permute(unsigned long long in, int in_bits,
                                      const unsigned char *table, int out_bits)
{
    unsigned long long out = 0;
    int i;
    for (i = 0; i < out_bits; i++)
        out = (out << 1) | ((in >> (in_bits - table[i])) & 1);
    return out;
}

static void des_subkeys(const UCHAR *key, unsigned long long sub[16])
{
    unsigned long long k = 0, cd;
    ULONG c, d;
    int i;
    for (i = 0; i < 8; i++) k = (k << 8) | key[i];
    cd = des_permute(k, 64, DES_PC1, 56);
    c = (ULONG)(cd >> 28) & 0x0FFFFFFF;
    d = (ULONG)cd & 0x0FFFFFFF;
    for (i = 0; i < 16; i++) {
        int s = DES_SHIFTS[i];
        c = ((c << s) | (c >> (28 - s))) & 0x0FFFFFFF;
        d = ((d << s) | (d >> (28 - s))) & 0x0FFFFFFF;
        sub[i] = des_permute(((unsigned long long)c << 28) | d, 56, DES_PC2, 48);
    }
}

static void des_block(const UCHAR *key, UCHAR *out, const UCHAR *in, int encrypt)
{
    unsigned long long sub[16], b = 0, x;
    ULONG l, r;
    int i, j;

    des_subkeys(key, sub);
    for (i = 0; i < 8; i++) b = (b << 8) | in[i];
    b = des_permute(b, 64, DES_IP, 64);
    l = (ULONG)(b >> 32); r = (ULONG)b;
    for (i = 0; i < 16; i++) {
        ULONG f = 0, t;
        x = des_permute(r, 32, DES_E, 48) ^ sub[encrypt ? i : 15 - i];
        for (j = 0; j < 8; j++) {
            ULONG six = (ULONG)(x >> (42 - 6 * j)) & 0x3F;
            ULONG row = ((six & 0x20) >> 4) | (six & 1), col = (six >> 1) & 0xF;
            f = (f << 4) | DES_S[j][row * 16 + col];
        }
        f = (ULONG)des_permute(f, 32, DES_P, 32);
        t = r; r = l ^ f; l = t;
    }
    b = des_permute(((unsigned long long)r << 32) | l, 64, DES_FP, 64);
    for (i = 7; i >= 0; i--) { out[i] = (UCHAR)b; b >>= 8; }
}

/* One 8-byte block through DES or EDE triple DES; the table holds the key. */
static void xc_block(ULONG cipher, UCHAR *out, const UCHAR *in, const UCHAR *keys, ULONG encrypt)
{
    if (cipher == 1) {
        UCHAR t[8];
        if (encrypt) {
            des_block(keys, t, in, 1);
            des_block(keys + 8, t, t, 0);
            des_block(keys + 16, out, t, 1);
        } else {
            des_block(keys + 16, t, in, 0);
            des_block(keys + 8, t, t, 1);
            des_block(keys, out, t, 0);
        }
    } else {
        des_block(keys, out, in, encrypt != 0);
    }
}

VOID __stdcall xbox_XcDESKeyParity(PUCHAR Key, ULONG KeyLength)
{
    ULONG i;
    for (i = 0; i < KeyLength; i++) {
        UCHAR b = Key[i] & 0xFE, p = b;
        p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
        Key[i] = b | (~p & 1);                       /* odd parity */
    }
}

VOID __stdcall xbox_XcKeyTable(ULONG CipherSelect, PVOID KeyTable, const UCHAR* Key)
{
    memcpy(KeyTable, Key, CipherSelect == 1 ? 24 : 8);
}

VOID __stdcall xbox_XcBlockCrypt(ULONG CipherSelect, PVOID Output, PVOID Input, PVOID KeyTable, ULONG Operation)
{
    xc_block(CipherSelect, (UCHAR *)Output, (const UCHAR *)Input,
             (const UCHAR *)KeyTable, Operation);
}

VOID __stdcall xbox_XcBlockCryptCBC(
    ULONG CipherSelect, ULONG OutputLength, PVOID Output, PVOID Input,
    PVOID KeyTable, ULONG Operation, PVOID FeedbackVector)
{
    UCHAR *out = (UCHAR *)Output, *fb = (UCHAR *)FeedbackVector;
    const UCHAR *in = (const UCHAR *)Input;
    ULONG off;
    int k;

    for (off = 0; off + 8 <= OutputLength; off += 8) {
        UCHAR blk[8], save[8];
        if (Operation) {
            for (k = 0; k < 8; k++) blk[k] = in[off + k] ^ fb[k];
            xc_block(CipherSelect, out + off, blk, (const UCHAR *)KeyTable, 1);
            memcpy(fb, out + off, 8);
        } else {
            memcpy(save, in + off, 8);
            xc_block(CipherSelect, blk, in + off, (const UCHAR *)KeyTable, 0);
            for (k = 0; k < 8; k++) out[off + k] = blk[k] ^ fb[k];
            memcpy(fb, save, 8);
        }
    }
}

VOID __stdcall xbox_XcCryptService(ULONG Operation, PVOID Param)
{
    (void)Operation;
    (void)Param;
}

VOID __stdcall xbox_XcUpdateCrypto(PVOID Param1, PVOID Param2)
{
    (void)Param1;
    (void)Param2;
}
