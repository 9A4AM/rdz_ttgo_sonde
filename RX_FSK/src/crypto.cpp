#include "crypto.h"

#define TAG "crypto"
#include "logger.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <Preferences.h>
#include <esp_random.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>

// PBKDF2 iteration count. The NVS pepper (HMAC with a 256-bit device key) is what actually
// protects a leaked /user.txt -- it's uncrackable without the key, regardless of iterations.
// PBKDF2 here is only light defense-in-depth, so a small count keeps login snappy.
#define PBKDF2_ITERS 2048

// Below this the device clock is considered "not set" (boot-relative time), so JWT
// expiry is not enforced. ~2023-11.
#define CLOCK_VALID_AFTER 1700000000UL

// base64url of {"alg":"HS256","typ":"JWT"} (the fixed JWT header).
static const char *JWT_HDR = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

// ---- device keys (NVS) ----

static uint8_t jwtKey[32];    static bool jwtKeyLoaded = false;
static uint8_t pepperKey[32]; static bool pepperLoaded = false;

static void loadOrCreateKey(const char *name, uint8_t *buf) {
  Preferences p;
  p.begin("rdzauth", false);
  if (p.getBytesLength(name) == 32) {
    p.getBytes(name, buf, 32);
  } else {
    esp_fill_random(buf, 32);
    p.putBytes(name, buf, 32);
    LOG_I(TAG, "generated new device key '%s'\n", name);
  }
  p.end();
}

const uint8_t *jwtSigningKey() {
  if (!jwtKeyLoaded) { loadOrCreateKey("jwtkey", jwtKey); jwtKeyLoaded = true; }
  return jwtKey;
}

const uint8_t *passwordPepperKey() {
  if (!pepperLoaded) { loadOrCreateKey("pepper", pepperKey); pepperLoaded = true; }
  return pepperKey;
}

bool deviceKeysExist() {
  Preferences p;
  if (!p.begin("rdzauth", true)) return false;   // namespace not created yet
  bool ok = (p.getBytesLength("pepper") == 32);
  p.end();
  return ok;
}

void rotateJwtKey() {
  esp_fill_random(jwtKey, 32);
  jwtKeyLoaded = true;
  Preferences p;
  p.begin("rdzauth", false);
  p.putBytes("jwtkey", jwtKey, 32);
  p.end();
  LOG_I(TAG, "rotated JWT signing key (all sessions invalidated)\n");
}

// ---- primitives ----

static void hmac256(const uint8_t *key, size_t kl, const uint8_t *d, size_t dl, uint8_t out[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  mbedtls_md_setup(&c, info, 1);   // 1 => HMAC
  mbedtls_md_hmac_starts(&c, key, kl);
  mbedtls_md_hmac_update(&c, d, dl);
  mbedtls_md_hmac_finish(&c, out);
  mbedtls_md_free(&c);
}

// PBKDF2-HMAC-SHA256 for a single 32-byte output block (implemented directly to avoid
// mbedtls API version differences). The HMAC context is set up once and reset per
// iteration (hmac_reset reuses the key schedule), instead of re-keying every round.
static void pbkdf2_sha256(const char *pw, const uint8_t *salt, size_t saltlen,
                          int iters, uint8_t out[32]) {
  uint8_t blk[80];                 // salt (<=64) || INT32BE(1)
  if (saltlen > sizeof(blk) - 4) saltlen = sizeof(blk) - 4;
  memcpy(blk, salt, saltlen);
  blk[saltlen] = 0; blk[saltlen + 1] = 0; blk[saltlen + 2] = 0; blk[saltlen + 3] = 1;

  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  mbedtls_md_setup(&c, info, 1);
  mbedtls_md_hmac_starts(&c, (const uint8_t *)pw, strlen(pw));   // key set once

  uint8_t U[32], T[32];
  mbedtls_md_hmac_update(&c, blk, saltlen + 4);
  mbedtls_md_hmac_finish(&c, U);
  memcpy(T, U, 32);
  for (int i = 1; i < iters; i++) {
    mbedtls_md_hmac_reset(&c);     // reuse the key schedule (ipad/opad)
    mbedtls_md_hmac_update(&c, U, 32);
    mbedtls_md_hmac_finish(&c, U);
    for (int j = 0; j < 32; j++) T[j] ^= U[j];
  }
  mbedtls_md_free(&c);
  memcpy(out, T, 32);
}

void genSalt(uint8_t *salt, size_t len) { esp_fill_random(salt, len); }

void hashPassword(const char *password, const uint8_t *salt, size_t saltlen, uint8_t out32[32]) {
  uint8_t pb[32];
  pbkdf2_sha256(password, salt, saltlen, PBKDF2_ITERS, pb);
  hmac256(passwordPepperKey(), 32, pb, 32, out32);   // pepper
}

void toHex(const uint8_t *in, size_t len, char *out) {
  static const char *h = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) { out[2 * i] = h[in[i] >> 4]; out[2 * i + 1] = h[in[i] & 0xf]; }
  out[2 * len] = 0;
}

int fromHex(const char *in, uint8_t *out, size_t maxout) {
  size_t n = 0;
  while (in[0] && in[1]) {
    if (n >= maxout) return -1;
    char hi = in[0], lo = in[1];
    int h = (hi >= '0' && hi <= '9') ? hi - '0' : (hi | 0x20) - 'a' + 10;
    int l = (lo >= '0' && lo <= '9') ? lo - '0' : (lo | 0x20) - 'a' + 10;
    if (h < 0 || h > 15 || l < 0 || l > 15) return -1;
    out[n++] = (h << 4) | l;
    in += 2;
  }
  return in[0] ? -1 : (int)n;   // odd length => error
}

// ---- base64url ----

static int b64url_enc(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char *)out, outcap, &olen, in, inlen) != 0) return -1;
  size_t w = 0;
  for (size_t i = 0; i < olen; i++) {
    char ch = out[i];
    if (ch == '+') ch = '-';
    else if (ch == '/') ch = '_';
    else if (ch == '=') continue;
    out[w++] = ch;
  }
  out[w] = 0;
  return (int)w;
}

static int b64url_dec(const char *in, size_t inlen, uint8_t *out, size_t outcap, size_t *outlen) {
  char tmp[384];
  if (inlen + 4 >= sizeof(tmp)) return -1;
  size_t w = 0;
  for (size_t i = 0; i < inlen; i++) {
    char ch = in[i];
    if (ch == '-') ch = '+';
    else if (ch == '_') ch = '/';
    tmp[w++] = ch;
  }
  while (w % 4) tmp[w++] = '=';
  tmp[w] = 0;
  return mbedtls_base64_decode(out, outcap, outlen, (const unsigned char *)tmp, w) == 0 ? 0 : -1;
}

// ---- JWT (HS256) ----

int jwtCreate(const char *user, int level, unsigned long ttlSec, char *out, int outlen) {
  unsigned long now = (unsigned long)time(NULL);
  unsigned long exp = (now > CLOCK_VALID_AFTER) ? now + ttlSec : 0;
  char payload[160];
  // username is bounded by the caller (user.txt records); cap defensively.
  char u[40];
  strlcpy(u, user, sizeof(u));
  snprintf(payload, sizeof(payload), "{\"sub\":\"%s\",\"lvl\":%d,\"exp\":%lu}", u, level, exp);

  char p64[256], s64[64], signin[320];
  if (b64url_enc((const uint8_t *)payload, strlen(payload), p64, sizeof(p64)) < 0) return -1;
  int sn = snprintf(signin, sizeof(signin), "%s.%s", JWT_HDR, p64);
  if (sn <= 0 || sn >= (int)sizeof(signin)) return -1;
  uint8_t sig[32];
  hmac256(jwtSigningKey(), 32, (const uint8_t *)signin, sn, sig);
  if (b64url_enc(sig, 32, s64, sizeof(s64)) < 0) return -1;
  int tn = snprintf(out, outlen, "%s.%s", signin, s64);
  return (tn > 0 && tn < outlen) ? tn : -1;
}

int jwtVerify(const char *jwt, char *userOut, int userOutLen, int *levelOut) {
  if (!jwt || !jwt[0]) return -1;
  const char *d1 = strchr(jwt, '.');
  if (!d1) return -1;
  const char *d2 = strchr(d1 + 1, '.');
  if (!d2) return -1;

  // Recompute the signature over header.payload and compare to the presented one.
  size_t silen = d2 - jwt;
  uint8_t sig[32];
  hmac256(jwtSigningKey(), 32, (const uint8_t *)jwt, silen, sig);
  char s64[64];
  int sl = b64url_enc(sig, 32, s64, sizeof(s64));
  const char *provided = d2 + 1;
  if (sl < 0 || strlen(provided) != (size_t)sl) return -1;
  unsigned diff = 0;
  for (int i = 0; i < sl; i++) diff |= (unsigned)(provided[i] ^ s64[i]);
  if (diff) return -1;

  // Signature valid -> decode the (trusted) payload and read our known fields.
  uint8_t payload[220];
  size_t plen = 0;
  if (b64url_dec(d1 + 1, d2 - (d1 + 1), payload, sizeof(payload) - 1, &plen) != 0) return -1;
  payload[plen] = 0;
  char *pj = (char *)payload;

  int level = -1;
  unsigned long exp = 0;
  char *l = strstr(pj, "\"lvl\":");
  if (l) level = atoi(l + 6);
  char *e = strstr(pj, "\"exp\":");
  if (e) exp = strtoul(e + 6, NULL, 10);
  if (userOut && userOutLen > 0) {
    userOut[0] = 0;
    char *s = strstr(pj, "\"sub\":\"");
    if (s) {
      s += 7;
      char *q = strchr(s, '"');
      if (q) {
        int n = q - s;
        if (n >= userOutLen) n = userOutLen - 1;
        memcpy(userOut, s, n);
        userOut[n] = 0;
      }
    }
  }

  unsigned long now = (unsigned long)time(NULL);
  if (exp > 0 && now > CLOCK_VALID_AFTER && now > exp) return -1;   // expired
  if (levelOut) *levelOut = level;
  return level;
}
