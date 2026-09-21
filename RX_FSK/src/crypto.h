#ifndef _CRYPTO_H
#define _CRYPTO_H

// Device-key-backed crypto for web authentication:
//  - two 32-byte keys live in NVS (flash key-value store, separate partition from
//    LittleFS so the file manager can't read them), generated on first use.
//  - jwtSigningKey signs stateless session tokens (JWT/HS256); rotate it to invalidate
//    every session at once ("log out everywhere").
//  - passwordPepperKey is mixed into stored password hashes so a leaked /user.txt is
//    useless without the device; it is NEVER rotated (that would orphan all hashes).

#include <stdint.h>
#include <stddef.h>

// Session token lifetime: long enough to "stay logged in" across reboots; only enforced
// once the device clock is set (NTP/GPS).
#define SESSION_TTL_SEC (7UL * 24 * 3600)

const uint8_t *jwtSigningKey();      // 32 bytes
const uint8_t *passwordPepperKey();  // 32 bytes
void rotateJwtKey();                 // invalidate all sessions

// True if the device password key already exists in NVS (i.e. not a fresh device). Does
// not create it. Used to decide whether a pre-existing /user.txt is still valid.
bool deviceKeysExist();

// Password hashing: out32 = HMAC-SHA256(pepper, PBKDF2-HMAC-SHA256(password, salt)).
void genSalt(uint8_t *salt, size_t len);
void hashPassword(const char *password, const uint8_t *salt, size_t saltlen, uint8_t out32[32]);

// Hex helpers. toHex writes 2*len chars + '\0'. fromHex returns bytes decoded or -1.
void toHex(const uint8_t *in, size_t len, char *out);
int fromHex(const char *in, uint8_t *out, size_t maxout);

// JWT (HS256, signed with jwtSigningKey). Payload: {"sub":user,"lvl":level,"exp":unix}.
// exp is omitted-as-0 when the device clock is not yet set, so a token still works after
// a reboot before time sync; once the clock is valid, exp is enforced.
// jwtCreate writes the token to out (returns length or -1). jwtVerify returns the access
// level (>=0) on a valid, unexpired, correctly-signed token, or -1 otherwise.
int jwtCreate(const char *user, int level, unsigned long ttlSec, char *out, int outlen);
int jwtVerify(const char *jwt, char *userOut, int userOutLen, int *levelOut);

#endif
