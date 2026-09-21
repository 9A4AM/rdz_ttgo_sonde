#ifndef _CONN_SERIALOUT_H
#define _CONN_SERIALOUT_H

#include "Sonde.h"
#include "conn.h"

class ConnSerialOut : public Conn {
public:
	void init();
	void netsetup();
	void netshutdown();
	void updateSonde( SondeInfo *si );
	void updateStation( PosInfo *pi );
	String getStatus();
	String getName();
private:
	bool isEnabled();
	bool started = false;
	unsigned long lastStationMs = 0;   // last station-line emit time (for ~1 Hz throttle)
	int begunTxd = -1;                 // pin/baud Serial1 was actually opened with at boot
	int begunBaud = 0;                 // (latched; changing config needs a reboot to take effect)
};

extern ConnSerialOut connSerialOut;

#endif
