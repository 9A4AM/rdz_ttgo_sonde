// Host test for ConnNotify::parseUrl. Build: g++ -std=c++11 -o /tmp/ptest scripts/test_notify_parseurl.cpp && /tmp/ptest
#include <cstdio>
#include <cstring>
#include <cassert>
#include <cstdlib>

static size_t strlcpy_local(char *d, const char *s, size_t n){ size_t l=strlen(s); if(n){ size_t c=l<n-1?l:n-1; memcpy(d,s,c); d[c]=0; } return l; }

// --- copy of the pure helper under test (keep identical to conn-notify.cpp) ---
// Parses url into (secure, host, port, path). Returns false on malformed input.
static bool parseUrl(const char *url, bool &secure, char *host, int hostlen, int &port, char *path, int pathlen) {
	if (!url) return false;
	const char *p = url;
	if (strncmp(p, "https://", 8) == 0) { secure = true;  port = 443; p += 8; }
	else if (strncmp(p, "http://", 7) == 0) { secure = false; port = 80; p += 7; }
	else return false;
	const char *slash = strchr(p, '/');
	const char *colon = strchr(p, ':');
	const char *hostend = slash;
	if (colon && (!slash || colon < slash)) hostend = colon;
	if (!hostend) hostend = p + strlen(p);
	int hl = (int)(hostend - p);
	if (hl <= 0 || hl >= hostlen) return false;
	memcpy(host, p, hl); host[hl] = 0;
	if (colon && (!slash || colon < slash)) {
		port = atoi(colon + 1);
		if (port <= 0) return false;
	}
	if (slash) strlcpy_local(path, slash, pathlen);
	else       strlcpy_local(path, "/", pathlen);
	return true;
}

int main() {
	bool sec; char host[96]; int port; char path[128];
	assert(parseUrl("https://ntfy.sh/mytopic", sec, host, 96, port, path, 128));
	assert(sec && !strcmp(host,"ntfy.sh") && port==443 && !strcmp(path,"/mytopic"));
	assert(parseUrl("http://192.168.1.5:8080/t", sec, host, 96, port, path, 128));
	assert(!sec && !strcmp(host,"192.168.1.5") && port==8080 && !strcmp(path,"/t"));
	assert(parseUrl("https://ntfy.sh", sec, host, 96, port, path, 128));
	assert(sec && !strcmp(host,"ntfy.sh") && port==443 && !strcmp(path,"/"));
	assert(!parseUrl("ftp://x/y", sec, host, 96, port, path, 128));
	assert(!parseUrl("garbage", sec, host, 96, port, path, 128));
	printf("PARSEURL-TESTS-PASS\n");
	return 0;
}
