
// Web authentication.
// Sessions are stateless JWTs (signed with the device key, see crypto.h): they are only
// verified, never stored, so logins survive reboots. COOKIE_SIZE must hold a JWT.
#define USERLEN 32
#define COOKIE_SIZE 320

enum { PERM_NONE, PERM_RO, PERM_ADMIN } permissions;

// Verify a JWT session token; returns the access level (0/1/2) or -1 if invalid/expired.
int getCookieAuthLevel(const char *cookie);

// Verify a plaintext password for `user`; returns the user's access level (>=0) or -1.
int verifyPassword(const char *user, const char *password);

int getDefaultAuthLevel();

// User management (named users in /user.txt; passwords stored hashed)
int setUser(const char *user, int level, const char *password);
int deleteUser(const char *user);
int getUserListJson(char *out, int outlen);
bool hasNamedUsers();
