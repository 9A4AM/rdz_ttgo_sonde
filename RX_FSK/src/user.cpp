#include "user.h"
#include "crypto.h"

#define TAG "user"
#include "logger.h"
#include <LittleFS.h>
#include <stdlib.h>
#include <string.h>

// Web authentication (stateless).
// - Passwords are stored hashed in /user.txt as "user,level,salt,hash" where
//   hash = HMAC(device-pepper, PBKDF2-HMAC-SHA256(password, salt)) (see crypto.cpp).
//   A leaked /user.txt cannot be cracked without the device's NVS key.
// - Sessions are stateless JWTs signed with the device key; we only verify them, nothing
//   is stored server-side, so a logged-in session survives a reboot until the token's
//   expiry (or until the JWT signing key is rotated, which logs everyone out).

extern int readLine(Stream &stream, char *buffer, int maxlen);  // impl in RX_FSK.ino

#define MAX_USER_LINE 200
#define SALT_BYTES 16
#define SALT_HEX (2 * SALT_BYTES + 1)
#define HASH_HEX 65                 // 32-byte hash as hex + '\0'

// Trim leading/trailing ASCII whitespace in place; returns a pointer to the first non-space char.
static char *trimws(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  int n = strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
  return s;
}

// Verify a JWT session token; returns the access level (0/1/2) or -1 if invalid/expired.
int getCookieAuthLevel(const char *cookie) {
  int level = -1;
  if (jwtVerify(cookie, NULL, 0, &level) < 0) return -1;
  return level;
}

// ---- /user.txt record lookup ----

// Look up a user's record (user,level,salt,hash), filling saltHex and credHex (the stored
// hash). saltHex is set to "" for a malformed/legacy row with no salt field. Returns true
// if the user was found.
static bool getUserRecord(const char *user, char *saltHex, int saltCap,
                          char *credHex, int credCap, int *outLevel) {
  File file = LittleFS.open("/user.txt", "r");
  if (!file) return false;
  char line[MAX_USER_LINE];
  bool found = false;
  while (file.available()) {
    int res = readLine(file, line, MAX_USER_LINE);
    if (res <= 0 || line[0] == '#') continue;
    char *s1 = strchr(line, ',');       if (!s1) continue; *s1 = 0;
    char *s2 = strchr(s1 + 1, ',');     if (!s2) continue; *s2 = 0;
    char *uname = trimws(line);
    if (strcmp(user, uname) != 0) continue;
    if (outLevel) {
      char *end; long v = strtol(trimws(s1 + 1), &end, 10);
      if (v < 0) v = 0;
      *outLevel = (int)v;
    }
    char *s3 = strchr(s2 + 1, ',');     // salt,hash separator (new format only)
    if (s3) {
      *s3 = 0;
      strlcpy(saltHex, trimws(s2 + 1), saltCap);
      strlcpy(credHex, trimws(s3 + 1), credCap);
    } else {
      saltHex[0] = 0;                   // legacy plaintext row
      strlcpy(credHex, trimws(s2 + 1), credCap);
    }
    found = true;
    break;
  }
  file.close();
  return found;
}

int verifyPassword(const char *user, const char *password) {
  char saltHex[SALT_HEX], cred[MAX_USER_LINE];
  int level = 0;
  if (!getUserRecord(user, saltHex, sizeof(saltHex), cred, sizeof(cred), &level)) return -1;
  if (saltHex[0] == 0) return -1;   // malformed/legacy row without a salt: not valid
  uint8_t salt[SALT_BYTES];
  if (fromHex(saltHex, salt, sizeof(salt)) != SALT_BYTES) return -1;
  uint8_t h[32];
  hashPassword(password, salt, SALT_BYTES, h);
  char hhex[HASH_HEX];
  toHex(h, 32, hhex);
  bool match = (strcmp(hhex, cred) == 0);
  LOG_I(TAG, "login: user '%s' match %d => level %d\n", user, match, match ? level : -1);
  return match ? level : -1;
}

int getDefaultAuthLevel() {
  // Anonymous (not-logged-in) access is derived purely from whether any user is
  // registered: full access (level 2) on a clean device with no users so it can be
  // set up, and no access (level 0) once the first account exists.
  return hasNamedUsers() ? 0 : 2;
}

// ---- User management (add/remove/list named users in /user.txt) ----

// Return the username field (up to the first comma, trimmed) of a record line.
static const char *lineUsername(const char *line, char *dst, int dstlen) {
  strlcpy(dst, line, dstlen);
  char *comma = strchr(dst, ',');
  if (comma) *comma = 0;
  return trimws(dst);
}

// Rewrite /user.txt via a temp file, applying one change to the record for `user`:
//  - newline != NULL : replace the matching line, or append it if `user` was not present
//  - newline == NULL : delete the matching line
// Comment lines, blank lines and all other records are preserved verbatim.
// Returns 1 if an existing record matched, 0 if appended/not-found, -1 on filesystem error.
static int rewriteUserFile(const char *user, const char *newline) {
  File in = LittleFS.open("/user.txt", "r");
  File out = LittleFS.open("/user.tmp", "w");
  if (!out) { if (in) in.close(); return -1; }
  bool matched = false;
  char line[MAX_USER_LINE];
  char namebuf[MAX_USER_LINE];
  if (in) {
    while (in.available()) {
      int n = readLine(in, line, MAX_USER_LINE);
      if (n <= 0) continue;                       // drop blank lines
      if (line[0] == '#') { out.printf("%s\n", line); continue; }  // keep comments
      const char *uname = lineUsername(line, namebuf, MAX_USER_LINE);
      if (strcmp(uname, user) == 0) {
        matched = true;
        if (newline) out.printf("%s\n", newline); // replace; for delete, write nothing
      } else {
        out.printf("%s\n", line);                 // keep other records verbatim
      }
    }
    in.close();
  }
  if (!matched && newline) out.printf("%s\n", newline);  // append new user
  out.close();
  LittleFS.remove("/user.txt");
  if (!LittleFS.rename("/user.tmp", "/user.txt")) return -1;
  return matched ? 1 : 0;
}

// Count named users with admin (level >= 2) access.
static int countAdmins() {
  File in = LittleFS.open("/user.txt", "r");
  if (!in) return 0;
  char line[MAX_USER_LINE];
  int count = 0;
  while (in.available()) {
    int n = readLine(in, line, MAX_USER_LINE);
    if (n <= 0 || line[0] == '#') continue;
    char *c1 = strchr(line, ',');     if (!c1) continue; *c1 = 0;
    char *c2 = strchr(c1 + 1, ',');   if (!c2) continue; *c2 = 0;
    if (trimws(line)[0] == 0) continue;
    if ((int)strtol(trimws(c1 + 1), NULL, 10) >= 2) count++;
  }
  in.close();
  return count;
}

// Current access level of a named user, or -1 if not present.
static int currentUserLevel(const char *user) {
  if (!hasNamedUsers()) return -1;
  char saltHex[SALT_HEX], cred[MAX_USER_LINE];
  int level = -1;
  if (getUserRecord(user, saltHex, sizeof(saltHex), cred, sizeof(cred), &level)) return level;
  return -1;
}

// Add or update a named user. Password is stored hashed (salt + PBKDF2 + pepper).
// Returns 0 on success, -1 on invalid input/error, -2 if it would demote the last administrator.
int setUser(const char *user, int level, const char *password) {
  if (!user || user[0] == 0 || !password) return -1;
  if (level < 1 || level > 2) return -1;
  // The first account must be an administrator, otherwise nobody could manage users again.
  bool firstUser = !hasNamedUsers();
  if (firstUser) level = 2;
  // Don't allow demoting the last administrator (would lock out user management).
  if (level < 2 && currentUserLevel(user) >= 2 && countAdmins() <= 1) return -2;
  // The flat file is comma-separated and line-based: reject usernames that would corrupt it.
  // (The password is hashed, never written verbatim, so it may contain any character.)
  if (strpbrk(user, ",\r\n")) return -1;
  if (strchr(user, '"')) return -1;                    // keep JSON listing simple/safe
  if ((int)strlen(user) + 2 * SALT_BYTES + 64 + 8 >= MAX_USER_LINE) return -1;

  uint8_t salt[SALT_BYTES];
  genSalt(salt, SALT_BYTES);
  uint8_t h[32];
  hashPassword(password, salt, SALT_BYTES, h);
  char saltHex[SALT_HEX], hashHex[HASH_HEX];
  toHex(salt, SALT_BYTES, saltHex);
  toHex(h, 32, hashHex);

  char newline[MAX_USER_LINE];
  snprintf(newline, MAX_USER_LINE, "%s,%d,%s,%s", user, level, saltHex, hashHex);
  if (rewriteUserFile(user, newline) < 0) return -1;
  return 0;
}

// Delete a named user.
// Returns 0 if removed, -1 if not found or on error, -2 if it would remove the last administrator.
int deleteUser(const char *user) {
  if (!user || user[0] == 0) return -1;
  if (currentUserLevel(user) >= 2 && countAdmins() <= 1) return -2;
  return rewriteUserFile(user, NULL) == 1 ? 0 : -1;
}

// Build a JSON array of named users (passwords/hashes never included): [{"user":"x","level":N},...]
int getUserListJson(char *out, int outlen) {
  int len = snprintf(out, outlen, "[");
  File in = LittleFS.open("/user.txt", "r");
  char line[MAX_USER_LINE];
  bool first = true;
  if (in) {
    while (in.available()) {
      int n = readLine(in, line, MAX_USER_LINE);
      if (n <= 0 || line[0] == '#') continue;
      char *c1 = strchr(line, ',');     if (!c1) continue; *c1 = 0;
      char *c2 = strchr(c1 + 1, ',');   if (!c2) continue; *c2 = 0;
      char *uname = trimws(line);
      if (uname[0] == 0) continue;
      int lvl = (int)strtol(trimws(c1 + 1), NULL, 10);
      if (len > outlen - 64) break;
      len += snprintf(out + len, outlen - len, "%s{\"user\":\"%s\",\"level\":%d}",
                      first ? "" : ",", uname, lvl);
      first = false;
    }
    in.close();
  }
  len += snprintf(out + len, outlen - len, "]");
  return len;
}

// True if /user.txt contains at least one named (non-empty username) user record.
bool hasNamedUsers() {
  File in = LittleFS.open("/user.txt", "r");
  if (!in) return false;
  char line[MAX_USER_LINE];
  char namebuf[MAX_USER_LINE];
  bool found = false;
  while (in.available()) {
    int n = readLine(in, line, MAX_USER_LINE);
    if (n <= 0 || line[0] == '#') continue;
    const char *uname = lineUsername(line, namebuf, MAX_USER_LINE);
    if (uname[0] != 0) { found = true; break; }
  }
  in.close();
  return found;
}
