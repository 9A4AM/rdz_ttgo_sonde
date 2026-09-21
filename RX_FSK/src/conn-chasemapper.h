#ifndef _CHASEMAPPER_H
#define _CHASEMAPPER_H

#include "Sonde.h"
#include "conn.h"

class ConnChasemapper : public Conn {
public:
	void init();
	void netsetup();
	void netshutdown();
	void updateSonde( SondeInfo *si );
	void updateStation( PosInfo *pi );
	bool replayReady();
        String getStatus();
	String getName();
};

extern ConnChasemapper connChasemapper;

#endif
