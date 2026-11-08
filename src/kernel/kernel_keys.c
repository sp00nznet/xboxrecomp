/*
 * kernel_keys.c - the console keys (XboxEEPROMKey, XboxHDKey,
 * XboxSignatureKey, XboxLANKey, XboxAlternateSignatureKeys) and the saves
 * zip's id of them.
 *
 * Apart from kernel_xbox.c so tests/saves_keys links the real arrays and
 * xbox_saves_keys_id with nothing but kernel_crypto.c: the in-game save
 * re-sign (enhance/saves_sign.c) signs under the zero key, which is right
 * only while these stay zero (pad-recording-ui R6, save safeguard 1).
 */

#include "kernel.h"

/* Crypto keys - zeroed, not needed for PC operation.
 * These are unique per-console on real hardware. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* A title signs its saves with these keys (XCalculateSignature mixes the
 * signature key with the title's; "noroam" saves add the HD key), so a save
 * moves between hosts only when they match. Every build leaves them zero;
 * the saves zip records which set made it (openspec overlay-saves, D1). */
const char *xbox_saves_keys_id(void)
{
    static char id[17];
    static const char hex[] = "0123456789abcdef";
    XBOX_SHA_CONTEXT sha;
    UCHAR d[20];
    int i, zero = 1;
    for (i = 0; i < 16; i++)
        if (xbox_HDKey[i] || xbox_SignatureKey[i])
            zero = 0;
    if (zero)
        return "zero";
    xbox_XcSHAInit(&sha);
    xbox_XcSHAUpdate(&sha, (const UCHAR *)"xbr-saves-keys", 14);
    xbox_XcSHAUpdate(&sha, xbox_HDKey, 16);
    xbox_XcSHAUpdate(&sha, xbox_SignatureKey, 16);
    xbox_XcSHAFinal(&sha, d);
    for (i = 0; i < 8; i++) {
        id[2 * i] = hex[d[i] >> 4];
        id[2 * i + 1] = hex[d[i] & 15];
    }
    id[16] = '\0';
    return id;
}
