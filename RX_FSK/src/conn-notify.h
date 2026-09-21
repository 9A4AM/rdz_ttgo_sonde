#ifndef _CONN_NOTIFY_H
#define _CONN_NOTIFY_H

#include "Sonde.h"
#include "conn.h"

#define NOTIFY_NEWSEEN 20   // serials remembered for the new-sonde alert (12 bytes each)

// config.notify.active is a bitfield of the alert kinds that are enabled.
#define NOTIFY_LANDING  1
#define NOTIFY_NEWSONDE 2

enum NtfyResult {
	NTFY_OK,          // delivered
	NTFY_TRANSIENT,   // connect failure / 5xx / 429 / timeout -- worth retrying
	NTFY_PERMANENT,   // bad config or 4xx -- retrying cannot fix it
};

class ConnNotify : public Conn {
public:
	void init();
	void netsetup();
	void netshutdown();
	void updateSonde( SondeInfo *si );
	void updateStation( PosInfo *pi );
	bool replayReady();
	String getStatus();
	String getName();

private:
	// Per-channel latch state (slot = si - sonde.sondeList, 0..MAXSONDE).
	char  lastSerial[MAXSONDE+1][12];
	bool  alerted[MAXSONDE+1];
	float peakAlt[MAXSONDE+1];
	unsigned long lastTry[MAXSONDE+1];   // millis() of the last landing-alert attempt (retry backoff)
	uint8_t lastTries[MAXSONDE+1];       // permanent failures for the current sonde (give-up cap)

	// New-sonde alert state. The ring is channel-agnostic on purpose, so a sonde that
	// moves between slots (scan mode) or is lost and reacquired doesn't alert twice.
	char  newSeen[NOTIFY_NEWSEEN][12];   // serials already alerted, oldest overwritten
	uint8_t newSeenPos;                  // next write position in newSeen
	// Backoff for the new-sonde alert. Connector-global rather than per-slot: autoscan
	// funnels every sonde through slot MAXSONDE and clears that slot's state on each
	// serial change, which would let a flipping autoscan lock defeat the rate limit.
	unsigned long lastNewTry;
	char  newPending[12];                // serial the give-up budget below applies to
	uint8_t newPendingTries;             // its permanent failures so far

	bool  initialized = false;
	char  laststatus[48] = "idle";

	// Blocking HTTP POST of one alert to server/topic. Composes the click URL from ser
	// and leaves "<kind> <ser> <outcome>" in laststatus.
	NtfyResult postNtfy(const char *kind, const char *ser, const char *title,
	                    const char *body, const char *tags, int priority);
	bool newSerialSeen(const char *ser);
	void rememberNewSerial(const char *ser);
	bool alertLanding(SondeInfo *si, const char *ser, int slot, float distkm);
	void alertNewSonde(SondeInfo *si, const char *ser, float distkm, bool hasdist);
};

extern ConnNotify connNotify;

#endif
