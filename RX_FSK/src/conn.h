/* 
 * conn.h
 * Interface for external data exporters
 * Copyright (c) 2023 Hansi Reiser, dl9rdz
 */

#ifndef conn_h
#define conn_h

#include "Sonde.h"


// to be moved elsewhere
struct PosInfo {
public:
	float lat;
	float lon;
};


/* Interface for all data exporters */
class Conn
{
public: 
        virtual ~Conn();

	/* Called once on startup */
	virtual void init() = 0;

	/* Called whenever the network becomes available */
	virtual void netsetup() = 0;

        /* Called when the network is shut down or when reconfiguration is forced, netshutdown connections... */
        virtual void netshutdown() = 0;

	/* Called approx 1x / second (maybe only if good data is available) */
	virtual void updateSonde( SondeInfo *si ) = 0;

	/* Called approx 1x / second* */
	virtual void updateStation( PosInfo *pi ) = 0;

	/* Offline upload cache hooks (default = opted out, for local/non-network sinks). */
	/* Return true when this connector's link is up and able to accept a frame now. */
	virtual bool replayReady() { return false; }
	/* Called once per tick when the drain delivered no frame (housekeeping/flush). */
	virtual void idleTick() {}
	/* Next cache seq this connector still owes; advanced by drainConnectors(). */
	uint32_t replayCursor = 0;

	///* Called whenever frequency changes (QRG details are in sondeList[nextSonde] */
	//virtual void updateQRG( int nextSonde ) = 0;

	/* Called to retrieve status (used for Info in about tab) */
	virtual String getStatus() = 0;

        /* Called to retrieve the name of this connector (for status display) */
	virtual String getName() = 0;

	static void appendUptime(char *str, int maxlen, uint32_t uptime);
	static void escapeJson(char *dst, const char *src, int maxlen);

};
#endif
