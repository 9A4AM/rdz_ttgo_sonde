#include "../features.h"
#if FEATURE_NOTIFY

#include "conn-notify.h"
#include "posinfo.h"
#include "logger.h"
#include <WiFiClient.h>

extern const char *sondeTypeStr[];
extern float calcLatLonDist(float lat1, float lon1, float lat2, float lon2);

#define NOTIFY_DESCENT_VS (-1.0f)   // m/s; below this counts as descending
#define NOTIFY_ALT_DROP_M (300.0f)  // m dropped from peak also counts as descending
#define NOTIFY_TIMEOUT_MS 5000
#define NOTIFY_RETRY_MS 60000       // min gap between send attempts after a failure
#define NOTIFY_MAX_TRIES 5          // give up (latch) after this many failed attempts

// Parse url into (secure, host, port, path). Returns false on malformed input.
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
	if (slash) strlcpy(path, slash, pathlen);
	else       strlcpy(path, "/", pathlen);
	return true;
}

void ConnNotify::init() {
	memset(lastSerial, 0, sizeof(lastSerial));
	memset(alerted, 0, sizeof(alerted));
	memset(lastTry, 0, sizeof(lastTry));
	memset(lastTries, 0, sizeof(lastTries));
	memset(newSeen, 0, sizeof(newSeen));
	newSeenPos = 0;
	lastNewTry = 0;
	memset(newPending, 0, sizeof(newPending));
	newPendingTries = 0;
	for (int i = 0; i <= MAXSONDE; i++) peakAlt[i] = -100000.0f;
	initialized = true;
}

bool ConnNotify::newSerialSeen(const char *ser) {
	for (int i = 0; i < NOTIFY_NEWSEEN; i++)
		if (newSeen[i][0] && strncmp(newSeen[i], ser, sizeof(newSeen[i])) == 0) return true;
	return false;
}

void ConnNotify::rememberNewSerial(const char *ser) {
	strlcpy(newSeen[newSeenPos], ser, sizeof(newSeen[newSeenPos]));
	newSeenPos = (newSeenPos + 1) % NOTIFY_NEWSEEN;
}

// Every exit records "<kind> <ser> <outcome>" in laststatus, so getStatus() says which sonde
// and which alert kind it refers to. The longest case fits: 5 + 11 + 26 + 2 < 48.
#define NOTIFY_STATUS(res, fmt, ...) do { \
		snprintf(laststatus, sizeof(laststatus), "%s %s " fmt, kind, ser, ##__VA_ARGS__); \
		return (res); \
	} while (0)

NtfyResult ConnNotify::postNtfy(const char *kind, const char *ser, const char *title,
                                const char *body, const char *tags, int priority) {
	bool secure; char host[96]; int port; char basepath[64];

	if (!parseUrl(sonde.config.notify.server, secure, host, sizeof(host), port, basepath, sizeof(basepath)))
		NOTIFY_STATUS(NTFY_PERMANENT, "bad server url");
	if (secure) {
		LOG_W("notify", "https server '%s' not supported (no TLS in firmware); use http://\n",
		      sonde.config.notify.server);
		NOTIFY_STATUS(NTFY_PERMANENT, "https unsupported");
	}

	// Compose full path: basepath (usually "/") + topic, avoiding a double slash.
	char path[128];
	if (basepath[strlen(basepath)-1] == '/')
		snprintf(path, sizeof(path), "%s%s", basepath, sonde.config.notify.topic);
	else
		snprintf(path, sizeof(path), "%s/%s", basepath, sonde.config.notify.topic);

	WiFiClient cl;
	// WiFiClient(=NetworkClient) does NOT override setTimeout, so this hits
	// Stream::setTimeout, whose unit is MILLISECONDS (it bounds readStringUntil below).
	// Passing seconds here would give a ~5 ms read window -> the status read times out ->
	// we report failure -> the (already-delivered) push re-fires. Must be milliseconds.
	cl.setTimeout(NOTIFY_TIMEOUT_MS);

	if (!cl.connect(host, port))
		NOTIFY_STATUS(NTFY_TRANSIENT, "connect failed");

	char clickurl[80];
	snprintf(clickurl, sizeof(clickurl), "https://sondehub.org/%s", ser);
	cl.printf("POST %s HTTP/1.1\r\n", path);
	if (port == 80) cl.printf("Host: %s\r\n", host);
	else            cl.printf("Host: %s:%d\r\n", host, port);   // vhost/proxy needs the port
	cl.print("Title: "); cl.print(title); cl.print("\r\n");
	cl.printf("Tags: %s\r\n", tags);
	cl.printf("Priority: %d\r\n", priority);
	cl.print("Click: "); cl.print(clickurl); cl.print("\r\n");
	if (sonde.config.notify.token[0])
		cl.printf("Authorization: Bearer %s\r\n", sonde.config.notify.token);
	cl.printf("Content-Length: %d\r\n", (int)strlen(body));
	cl.print("Connection: close\r\n\r\n");
	cl.print(body);

	// Read status line (best-effort, bounded by timeout).
	String status = cl.readStringUntil('\n');
	cl.stop();
	// "HTTP/1.x CODE ..." -> extract CODE. 2xx = delivered; 4xx (except 429 rate-limit)
	// is a permanent client/config error; anything else is transient.
	int code = 0, sp = status.indexOf(' ');
	if (sp > 0) code = status.substring(sp + 1).toInt();
	if (code >= 200 && code < 300) NOTIFY_STATUS(NTFY_OK, "sent");
	if (code >= 400 && code < 500 && code != 429) NOTIFY_STATUS(NTFY_PERMANENT, "http %d", code);
	NOTIFY_STATUS(NTFY_TRANSIENT, "http %d", code);
}

#undef NOTIFY_STATUS

void ConnNotify::netsetup() {}
void ConnNotify::netshutdown() {}
void ConnNotify::updateStation(PosInfo *pi) {}

// Opt out of the cache drain: this connector is a live-only sink, fed the current frame directly
// like the SD card and the serial output. Backfill has nothing to offer it. A replayed frame
// arrives as a stack temp with no channel slot, and every per-sonde value the alerts need --
// peakAlt for the descent signal, the alerted latch, the backoff -- is indexed by slot, so
// updateSonde() drops those frames. Taking part in the drain would only mean that, while the
// connector is behind on backfill, it burns its whole quota on frames it discards and never
// reaches the live one -- delaying an alert or missing it outright.
bool ConnNotify::replayReady() {
	return false;
}

// Announce a sonde the first time it is decoded with a position. The serial is only written
// into the seen-ring once the push is delivered, so a network outage doesn't burn the alert.
void ConnNotify::alertNewSonde(SondeInfo *si, const char *ser, float distkm, bool hasdist) {
	unsigned long nowms = millis();
	if (lastNewTry != 0 && (nowms - lastNewTry) < NOTIFY_RETRY_MS) return;
	lastNewTry = nowms;

	char distinfo[24];
	if (hasdist) snprintf(distinfo, sizeof(distinfo), ", %.1f km away", distkm);
	else         distinfo[0] = 0;

	uint8_t rt = si->type;
	char title[80], body[256];
	snprintf(title, sizeof(title), "New sonde: %s", ser);
	snprintf(body, sizeof(body),
	         "%s %s\n%.3f MHz%s, alt %d m, RSSI %.1f dBm\nlat %.4f, lon %.4f",
	         (rt < NSondeTypes ? sondeTypeStr[rt] : "?"), ser,
	         si->freq, distinfo, (int)si->d.alt, -si->rssi / 2.0,
	         si->d.lat, si->d.lon);

	NtfyResult res = postNtfy("new", ser, title, body, "balloon", 3);
	if (res == NTFY_OK) {
		rememberNewSerial(ser);
	} else if (res == NTFY_PERMANENT) {
		// Give-up budget is per serial, matching what the ring is keyed by: a single counter
		// would let sondes A..D burn it and then strand E after one failure of its own.
		if (strncmp(newPending, ser, sizeof(newPending)) != 0) {
			strlcpy(newPending, ser, sizeof(newPending));
			newPendingTries = 0;
		}
		if (++newPendingTries >= NOTIFY_MAX_TRIES) {
			rememberNewSerial(ser);   // give up rather than retry an unfixable failure forever
			newPending[0] = 0;
			LOG_W("notify", "giving up on new-sonde alert for %s (%s)\n", ser, laststatus);
		}
	}
	// (transient failures: not remembered, not counted -- retried on the next backoff window)
	LOG_I("notify", "%s\n", laststatus);
}

void ConnNotify::updateSonde(SondeInfo *si) {
	if (!sonde.config.notify.active) return;
	if (sonde.config.notify.topic[0] == 0) return;
	if (!initialized) init();

	// Replayed cache frames arrive via a stack temp (drainConnectors), not a sondeList
	// element, and are skipped here on purpose: we only alert on the live frame, never on
	// backfilled history. Address equality is what makes that reliable -- deriving the index
	// as si - sondeList is UB for those temps (pointers into unrelated objects), and a
	// bounds test on the result only rejects them while the stack and sondeList happen to
	// sit more than MAXSONDE+1 elements apart in DRAM.
	int slot = -1;
	for (int i = 0; i <= MAXSONDE; i++)
		if (si == &sonde.sondeList[i]) { slot = i; break; }
	if (slot < 0) return;

	// Identity: ser and id are always written together by the decoders (RS41 sets both
	// to the same serial; DFM/M10 derive ser from id), so this never flips between two
	// different values -- only empty<->serial. Skip unidentified frames (e.g. a DFM
	// bad-frame clear zeroes ser/id) so an empty key can't overwrite the stored serial
	// and spuriously reset the latch/peak.
	const char *ser = (si->d.ser[0]) ? si->d.ser : si->d.id;
	if (ser[0] == 0) return;
	// New (different, identified) sonde in this slot? reset all per-sonde state, including
	// the backoff timer/try-count -- otherwise a fresh sonde would inherit the previous
	// sonde's backoff and have its first alert suppressed for up to NOTIFY_RETRY_MS.
	if (strncmp(lastSerial[slot], ser, sizeof(lastSerial[slot])) != 0) {
		strlcpy(lastSerial[slot], ser, sizeof(lastSerial[slot]));
		alerted[slot] = false;
		peakAlt[slot] = -100000.0f;
		lastTry[slot] = 0;
		lastTries[slot] = 0;
	}

	// Need a fresh, complete fix. VALIDPOS only covers lat/lon; altitude and vertical
	// speed have their own bits (DFM sets them independently, e.g. clears VALIDALT when
	// alt==0). Require valid altitude, and skip stale ("old", 0x80) positions, so we never
	// evaluate "landing" against a bogus alt=0 or a reused last-known fix.
	if (!VALIDPOS(si->d.validPos)) return;
	if (!VALIDALT(si->d.validPos)) return;
	if (si->d.validPos & 0x80) return;

	if (si->d.alt > peakAlt[slot]) peakAlt[slot] = si->d.alt;

	// Station position: GPS if valid, else fixed rxlat/rxlon. Without it there is no
	// distance: the landing alert cannot be evaluated at all, the new-sonde alert just
	// omits the distance line.
	float mylat = sonde.config.rxlat, mylon = sonde.config.rxlon;
	bool hasdist = !(isnan(mylat) || isnan(mylon));
	if (gpsPos.valid) { mylat = gpsPos.lat; mylon = gpsPos.lon; hasdist = true; }
	float distkm = hasdist ? calcLatLonDist(mylat, mylon, si->d.lat, si->d.lon) / 1000.0f : 0.0f;

	// Landing goes first and, when it sends, takes this frame's send window: it is the
	// time-critical alert (the sonde may stop transmitting within the minute) while the
	// new-sonde push can wait for the next frame. This also keeps at most one blocking
	// connect+POST per updateSonde() call.
	bool sent = (sonde.config.notify.active & NOTIFY_LANDING) && hasdist &&
	            alertLanding(si, ser, slot, distkm);

	// First positioned frame of a serial we have not announced yet. Deliberately not gated
	// on distance or altitude: the receiver only hears what is in range, so every newly
	// identified sonde is worth announcing.
	if (!sent && (sonde.config.notify.active & NOTIFY_NEWSONDE) && !newSerialSeen(ser))
		alertNewSonde(si, ser, distkm, hasdist);
}

// Alert on a sonde that is near, low and descending. Returns true when a send was attempted,
// so the caller can keep the new-sonde push off the same frame.
bool ConnNotify::alertLanding(SondeInfo *si, const char *ser, int slot, float distkm) {
	bool near = distkm <= (float)sonde.config.notify.dist;
	// Trust vs only when its validity bit is set; the altitude drop from peak is the
	// fallback descent signal when vs is missing/unreliable.
	bool descending = (VALIDVS(si->d.validPos) && si->d.vs < NOTIFY_DESCENT_VS) ||
	                  ((peakAlt[slot] - si->d.alt) > NOTIFY_ALT_DROP_M);
	bool low = si->d.alt < (float)sonde.config.notify.alt * 1000.0f;

	if (!(near && descending && low)) return false;
	if (alerted[slot]) return false;

	// Cross-channel dedup: if this serial was already alerted in another slot (e.g. the
	// same frequency configured on two channels), don't alert again for it.
	for (int j = 0; j <= MAXSONDE; j++) {
		if (j != slot && alerted[j] && strncmp(lastSerial[j], ser, sizeof(lastSerial[j])) == 0) {
			alerted[slot] = true;
			return false;
		}
	}

	// Backoff: at most one send attempt per NOTIFY_RETRY_MS. Otherwise a persistently
	// unreachable/erroring server would re-run a blocking connect+POST on every frame
	// (~1/s) while a qualifying sonde is in view, stalling the UI/web loop.
	unsigned long nowms = millis();
	if (lastTry[slot] != 0 && (nowms - lastTry[slot]) < NOTIFY_RETRY_MS) return false;
	lastTry[slot] = nowms;

	// Build body + title. Only include the vertical rate when it's actually valid
	// (descent may have been detected via the altitude-drop fallback, with vs unset);
	// printing an invalid vs would show a garbage "descending N m/s".
	uint8_t rt = si->type;
	char vsinfo[32];
	if (VALIDVS(si->d.validPos))
		snprintf(vsinfo, sizeof(vsinfo), ", %s %.1f m/s",
		         (si->d.vs < 0 ? "descending" : "climbing"),
		         (si->d.vs < 0 ? -si->d.vs : si->d.vs));
	else
		vsinfo[0] = 0;
	char title[80], body[256];
	snprintf(title, sizeof(title), "Sonde landing near: %s", ser);
	snprintf(body, sizeof(body),
	         "%s %s\n%.1f km away, alt %d m%s\nlat %.4f, lon %.4f",
	         (rt < NSondeTypes ? sondeTypeStr[rt] : "?"), ser,
	         distkm, (int)si->d.alt, vsinfo,
	         si->d.lat, si->d.lon);

	// Send. Outcomes:
	//   ok        -> delivered; latch (one alert per sonde).
	//   permanent -> a failure retrying can't fix (bad/https config, or HTTP 4xx except
	//                429); counts toward NOTIFY_MAX_TRIES, then we give up.
	//   transient -> connect failure / 5xx / 429 / timeout; does NOT count -- keep retrying
	//                on the backoff (bounded anyway by the sonde leaving view), so a WiFi
	//                outage during descent doesn't permanently burn the alert.
	NtfyResult res = postNtfy("land", ser, title, body, "balloon,warning", 4);
	if (res == NTFY_OK) {
		alerted[slot] = true;   // delivered: one alert per sonde
	} else if (res == NTFY_PERMANENT) {
		if (lastTries[slot] < 255) lastTries[slot]++;
		if (lastTries[slot] >= NOTIFY_MAX_TRIES) {
			alerted[slot] = true;   // give up on an unfixable failure rather than retry forever
			LOG_W("notify", "giving up on %s after %d permanent failures (%s)\n",
			      ser, (int)lastTries[slot], laststatus);
		}
	}
	// (transient failures: no latch, no count -- retried on the next backoff window)
	LOG_I("notify", "%s (%.1fkm)\n", laststatus, distkm);
	return true;
}

String ConnNotify::getStatus() {
	if (!sonde.config.notify.active) return String("disabled");
	if (sonde.config.notify.topic[0] == 0) return String("no topic set");
	return String(laststatus);
}

String ConnNotify::getName() { return String("Notify"); }

ConnNotify connNotify;
#endif
