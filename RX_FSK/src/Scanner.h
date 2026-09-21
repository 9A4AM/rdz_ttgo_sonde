

#ifndef _SCANNER_H
#define _SCANNER_H

#include <stdlib.h>
#include <stdint.h>
#include <Arduino.h>
#ifndef inttypes_h
        #include <inttypes.h>
#endif
// One detected spectrum peak (used by auto-scan). freqHz is the RF frequency
// (quantized to the configured step); power is in scanresult[] units (-RssiValue,
// i.e. 2*dBm: higher = stronger).
struct ScanPeak {
	double freqHz;
	int power;
};

class Scanner
{
private:
	void fillTiles(uint8_t *row, int value);

public:
	void plotResult();
	void scan(void);
	// Extract up to maxpeaks peaks from the most recent scan, strongest first.
	// snr_db: required level above the (median) noise floor; mindist_hz: minimum
	// spacing between peaks; quant_hz: snap peak frequencies to this step (also
	// dedups peaks landing on the same channel). Returns the number of peaks.
	int findPeaks(ScanPeak *out, int maxpeaks, int snr_db, int mindist_hz, int quant_hz);
	// Web scan-plot support
	void scanForWeb();		// data-only sweep (no display), bumps the sequence counter
	uint32_t webSeq();		// increments on each completed web sweep
	uint32_t webMillis();		// millis() at the last web sweep (0 if none yet)
	int dispW();			// number of display bins in scandisp[]
	double stepMHz();		// MHz per display bin
	const int *dispData();		// pointer to scandisp[] (dispW() entries)
	double peakMHz();		// peak frequency in MHz
};

extern Scanner scanner;
#endif
