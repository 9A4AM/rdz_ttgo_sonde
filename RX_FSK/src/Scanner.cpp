#include "Scanner.h"
#include <inttypes.h>
#include <U8x8lib.h>

#include "SX1278FSK.h"
#include "Sonde.h"
#include "Display.h"
#include "src/conn-mqtt.h"


double STARTF;


struct scancfg {
	int PLOT_W;		// Width of plot, in pixel
	int PLOT_H8;		// Height of plot, in 8 pixel units
	int TICK1;		// Pixel per MHz marker
	int TICK2;		// Pixel per sub-Mhz marker (250k or 200k)
	double CHANSTEP;	// Scanner frequenz steps
	int SMPL_PIX;		// Frequency steps per pixel
	int NCHAN;		// number of channels to scan, PLOT_W * SMPL_PIX
	int SMOOTH;
	int ADDWAIT;
	int VSCALE;
};

//struct scancfg scanLCD={ 121, 7,  120/6, 120/6/4, 6000.0/120.0/20.0, 20, 120*20, 1 };
struct scancfg scanLCD={ 121, 7,  120/6, 120/6/4, 6000.0/120.0/10.0, 10, 120*10, 2, 40, 1 };
// 220x176
struct scancfg scanTFT={ 210, 16, 210/6, 210/6/5, 6000.0/210.0/10.0, 10, 210*10, 1, 0, 1 };
// 320x240
struct scancfg scan934x={ 300, 22, 300/6, 300/6/5, 6000.0/300.0/7.0, 7, 300*5, 1, 10, 2 };
// 480x320
struct scancfg scan7796={ 481, 35, 480/6, 480/6/5, 6000.0/480.0/5.0, 5, 480*5, 1, 10, 3 };
struct scancfg &scanconfig = scanTFT;

#define CHANBW 12500
//#define PIXSAMPL (50/CHANBW)
//#define STARTF 401000000

// max of 120*5 and 210*3
//#define MAXN 210*10
//#define MAXN 120*20
//#define MAXN 300*10
#define MAXN 481*5

// max of 120 and 210 (ceil(210/8)*8)) -- now ceil(300/8)*8
//#define MAXDISP 216
//#define MAXDISP 304
#define MAXDISP 488

int scanresult[MAXN];
int scandisp[MAXDISP];
double peakf=0;
static volatile uint32_t scanWebSeq = 0;
static volatile uint32_t scanWebMillis = 0;

//#define PLOT_MIN -250
#define PLOT_MIN (sonde.config.noisefloor*2)
#define PLOT_SCALE(x) (x<PLOT_MIN?0:(x-PLOT_MIN)/2)

const byte tilepatterns[9]={0,0x80,0xC0,0xE0,0xF0,0xF8,0xFC,0xFE,0xFF};
void Scanner::fillTiles(uint8_t *row, int value) {
	for(int y=0; y<scanconfig.PLOT_H8; y++) {
		int nbits = scanconfig.VSCALE*value - 8*(scanconfig.PLOT_H8-1-y);
		if(nbits<0) { row[8*y]=0; continue; }
		if(nbits>=8) { row[8*y]=255; continue; }
		row[8*y] = tilepatterns[nbits];
	}
}
/* LCD:
 * There are 16*8 columns to plot, NPLOT must be lower than that
 * currently, we use 128 * 50kHz channels
 * There are 8*8 values to plot; MIN is bottom end, 
 * TFT:
 * There are 210 columns to plot
 * Currently we use 210 * (6000/120)kHz channels, i.e. 28.5714kHz
 */
///// unused????  uint8_t tiles[16] = { 0x0f,0x0f,0x0f,0x0f,0xf0,0xf0,0xf0,0xf0, 1, 3, 7, 15, 31, 63, 127, 255};

// type 0: lcd, 1: tft(ILI9225), 2: lcd(sh1106) 3:TFT(ili9341), 4: TFT(ili9342), 5: TFT(ST7789), 6:ST7796
#define ISTFT (sonde.config.disptype!=0 && sonde.config.disptype!=2)
void Scanner::plotResult()
{
	int yofs = 0;
	char buf[30];
	// startfreq may be a decimal (or NaN if the config field was cleared); fall back to 400.
	double sf = isnan(sonde.config.startfreq) ? 400.0 : sonde.config.startfreq;
	if(ISTFT) {
		yofs = 2;
  		if (sonde.config.marker != 0) {
    			snprintf(buf, sizeof(buf), "%g", sf);
    			disp.rdis->drawString(0, 1, buf);
    			disp.rdis->drawString(scanconfig.PLOT_W/2-9, 1, "MHz");
    			snprintf(buf, sizeof(buf), "%g", sf + 6);
    			disp.rdis->drawString(scanconfig.PLOT_W-18, 1, buf);
		}	
	}
	else {
  		if (sonde.config.marker != 0) {
    			snprintf(buf, sizeof(buf), "%g", sf);
    			disp.rdis->drawString(0, 1, buf);
    			disp.rdis->drawString(7, 1, "MHz");
    			snprintf(buf, sizeof(buf), "%g", sf + 6);
    			disp.rdis->drawString(13, 1, buf);
		}	
  	}
	uint8_t row[scanconfig.PLOT_H8*8];
	if(ISTFT) {
		Arduino_GFX *gfx = static_cast<ILI9225Display *>(disp.rdis)->tft;
        	for(int i=0; i<scanconfig.PLOT_W; i++) {
			// Draw spectrum using optimized TFT function from Arduino_GFX
			uint16_t value = scanconfig.VSCALE * PLOT_SCALE(scandisp[i]);
			uint16_t barpos = scanconfig.PLOT_H8 * 8 - value;
			// blue line from top to value
			gfx->drawFastVLine(i, 8*yofs, barpos, BLUE);
			gfx->drawFastVLine(i, 8*yofs+barpos, value, GREEN);

			// draw tick marks if needed
			if( (i%scanconfig.TICK1)==0 ) { 
				gfx->drawFastVLine(i, 8*yofs, 3, GREEN);
			} else if( (i%scanconfig.TICK2)==0 ) {
				gfx->drawFastVLine(i, 8*yofs, 1, GREEN);
			}
		}
        } else {
	    for(int i=0; i<scanconfig.PLOT_W; i+=8) {
		for(int j=0; j<8; j++) {
			fillTiles(row+j, PLOT_SCALE(scandisp[i+j]));
			if( (i+j)>=scanconfig.PLOT_W ) { for(int y=0; y<scanconfig.PLOT_H8; y++) row[j+8*y]=0; }
		        if( ((i+j)%scanconfig.TICK1)==0) { row[j] |= 0x07; }
		        if( ((i+j)%scanconfig.TICK2)==0) { row[j] |= 0x01; }
		}
		for(int y=0; y<scanconfig.PLOT_H8; y++) {
			if(sonde.config.marker && y==1 && !ISTFT ) {
				// don't overwrite MHz marker text
				if(i<3*8 || (i>=7*8&&i<10*8) || i>=13*8) continue;
			}
			disp.rdis->drawTile(i/8, y+yofs, 1, row+8*y);
		}
	    }
        }
	if(ISTFT) { // large TFT
		sprintf(buf, "Peak: %03.3f MHz", peakf*0.000001);	
		disp.rdis->drawString(0, (yofs+scanconfig.PLOT_H8+1)*8, buf);
	} else {
		sprintf(buf, "Peak: %03.3fMHz", peakf*0.000001);	
		disp.rdis->drawString(0, 7, buf);
	}
}

void Scanner::scan()
{
	if(!ISTFT) { // LCD small
		scanconfig = scanLCD;
	} else if (sonde.config.disptype==1) {
		scanconfig = scanTFT;
	} else if (sonde.config.disptype==6) {
		scanconfig = scan7796;
	} else {
		scanconfig = scan934x;
	}
	// Optional per-bin dwell override (config). Only the RSSI timing is changed;
	// the plot geometry (PLOT_W/CHANSTEP/SMPL_PIX) stays from the display profile,
	// so the on-device scan plot keeps rendering unchanged. A longer dwell gives
	// steadier RSSI / better weak-peak detection at the cost of a slower sweep.
	// -1 = keep the display default.
	if(sonde.config.scan_smooth >= 0) scanconfig.SMOOTH = sonde.config.scan_smooth & 0x07;
	if(sonde.config.scan_addwait >= 0) scanconfig.ADDWAIT = sonde.config.scan_addwait;
	// Configure
 	// startfreq may be a decimal (or NaN if the config field was cleared); fall back to 400 MHz.
 	STARTF = ((isnan(sonde.config.startfreq) ? 400.0 : sonde.config.startfreq) * 1000000);
	sx1278.writeRegister(REG_PLL_HOP, 0x80);   // FastHopOn
	sx1278.setRxBandwidth((int)(scanconfig.CHANSTEP*1000));
	double bw = sx1278.getRxBandwidth();
	Serial.print("RX Bandwith for scan: "); Serial.println(bw);
	sx1278.writeRegister(REG_RSSI_CONFIG, scanconfig.SMOOTH&0x07);
	sx1278.setFrequency(STARTF);
	Serial.print("Start freq = "); Serial.println(STARTF);
	sx1278.writeRegister(REG_OP_MODE, FSK_RX_MODE);

	unsigned long start = millis();
	uint32_t lastfrf= STARTF * (1<<19) / SX127X_CRYSTAL_FREQ;
	float freq = STARTF;
	int wait = scanconfig.ADDWAIT + 20 + 1000*(1<<(scanconfig.SMOOTH+1))/4/(0.001*CHANBW);
	Serial.print("wait time (us) is: "); Serial.println(wait);
	// Number of full sweeps; the max RSSI per bin is kept across them. More passes
	// spread the per-bin revisits over more wall-clock time, so a periodic signal
	// (e.g. RS41, ~1 frame/s) is caught even when it is off during some passes.
	int niter = sonde.config.scan_iter;
	if(niter < 1) niter = 1; else if(niter > 20) niter = 20;
	for(int iter=0; iter<niter; iter++) {   // multiple iterations, to catch all RS41 transmissions
	    delayMicroseconds(20000); yield();
	    for(int i=0; i<scanconfig.PLOT_W*scanconfig.SMPL_PIX; i++) {
		freq = STARTF + 1000.0*i*scanconfig.CHANSTEP;
		//freq = 404000000 + 100*i*scanconfig.CHANSTEP;
		uint32_t frf = freq * 1.0 * (1<<19) / SX127X_CRYSTAL_FREQ;
		if( (lastfrf>>16)!=(frf>>16) ) {
        		sx1278.writeRegister(REG_FRF_MSB, (frf&0xff0000)>>16);
		}
		if( ((lastfrf&0x00ff00)>>8) != ((frf&0x00ff00)>>8) ) {
        		sx1278.writeRegister(REG_FRF_MID, (frf&0x00ff00)>>8);
		}
        	sx1278.writeRegister(REG_FRF_LSB, (frf&0x0000ff));
		lastfrf = frf;
		// Wait TS_HOP (20us) + TS_RSSI ( 2^(scacconfig.SMOOTH+1) / 4 / CHANBW us)
		delayMicroseconds(wait);
		int rssi = -(int)sx1278.readRegister(REG_RSSI_VALUE_FSK);
		// A register read of 0 ("0 dBm") is a rail artifact (e.g. the PLL not yet
		// settled after a coarse-frequency step), not a real signal. Clamp it to
		// the noise floor so it neither spikes the plot nor looks like a peak.
		if(rssi >= 0) rssi = sonde.config.noisefloor * 2;
		if(iter==0) { scanresult[i] = rssi; } else {
			if(rssi>scanresult[i]) scanresult[i]=rssi;
		}
	    }
	}
	yield();
	unsigned long duration = millis()-start;
	Serial.print("wait: ");
	Serial.println(wait);
	Serial.print("Scan time: ");
	Serial.println(duration);
	Serial.print("Final freq: ");
	Serial.println(freq);
	int peakidx=-1;
	int peakres=-9999;
	for(int i=0; i<scanconfig.PLOT_W; i+=1) {
		int r=scanresult[i*scanconfig.SMPL_PIX];
		if(r>peakres+1) { peakres=r; peakidx=i*scanconfig.SMPL_PIX; }
		// Accumulate in a local and store scandisp[i] only once, as the finished
		// average. Writing the running sum into scandisp[] and dividing in a
		// second pass would leave a window where the web task (createSpectrumJson)
		// reads the un-divided sum (~SMPL_PIX times too negative).
		int sum = r;
		for(int j=1; j<scanconfig.SMPL_PIX; j++) {
			r = scanresult[i*scanconfig.SMPL_PIX+j];
			sum += r;
			if(r>peakres+1) { peakres=r; peakidx=i*scanconfig.SMPL_PIX+j; }
		}
		scandisp[i] = sum / scanconfig.SMPL_PIX;
		Serial.print(scanresult[i]); Serial.print(", ");
	}
	peakidx--;
	double newpeakf = STARTF + scanconfig.CHANSTEP*1000.0*peakidx;
	if(newpeakf<peakf-20000 || newpeakf>peakf+20000) peakf=newpeakf; 		// different frequency
	else if (newpeakf < peakf) peakf = 0.75*newpeakf + 0.25*peakf;		// averaging on frequency, some bias towards lower...
	else peakf = 0.25*newpeakf + 0.75*peakf;
	Serial.println("\n");
	for(int i=0; i<scanconfig.PLOT_W; i++) {
                Serial.print(scandisp[i]); Serial.print(", ");
	}
	Serial.println("\n");
	Serial.printf("Peak: %f rssi %d\n", peakf, peakres);
#if FEATURE_MQTT
	connMQTT.publishPeak(peakf, peakres);
#endif
}

int Scanner::findPeaks(ScanPeak *out, int maxpeaks, int snr_db, int mindist_hz, int quant_hz)
{
	int N = scanconfig.PLOT_W * scanconfig.SMPL_PIX;
	if (N > MAXN) N = MAXN;
	if (N < 1 || maxpeaks < 1) return 0;
	if (maxpeaks > 64) maxpeaks = 64;
	if (quant_hz < 1) quant_hz = 1;

	// Some frequencies (typically the band edges) read the RSSI register as 0,
	// i.e. "0 dBm" — a rail artifact, not a real signal. Treat any non-negative
	// reading (scanresult >= 0) as invalid so it neither skews the noise floor
	// nor gets picked as a (very strong) peak.
	#define SCAN_VALID(i) (scanresult[i] < 0)

	// Estimate the noise floor as the median of the valid bins, via a histogram
	// over reg = -scanresult[i] (1..255). Robust because most bins are noise.
	int hist[256];
	for (int i = 0; i < 256; i++) hist[i] = 0;
	int validN = 0;
	for (int i = 0; i < N; i++) {
		if (!SCAN_VALID(i)) continue;
		int reg = -scanresult[i];
		if (reg > 255) reg = 255;
		hist[reg]++;
		validN++;
	}
	if (validN < 1) return 0;			// no usable spectrum data
	int medianReg = 0, cum = 0;
	for (int r = 0; r < 256; r++) { cum += hist[r]; if (cum >= validN / 2) { medianReg = r; break; } }
	int noisefloorVal = -medianReg;			// scanresult units (= 2*dBm)
	int threshold = noisefloorVal + 2 * snr_db;	// SNR in dB -> 2x in these units

	double hzPerBin = 1000.0 * scanconfig.CHANSTEP;
	int mindistBins = (int)(mindist_hz / hzPerBin);
	if (mindistBins < 1) mindistBins = 1;

	int selBin[64];
	int npk = 0;
	// Greedy: repeatedly take the strongest bin above threshold that is not within
	// mindist of, nor on the same quantized channel as, an already-selected peak.
	for (int k = 0; k < maxpeaks; k++) {
		int bestBin = -1, bestVal = threshold;
		for (int i = 0; i < N; i++) {
			if (!SCAN_VALID(i)) continue;		// skip rail/invalid readings
			int v = scanresult[i];
			if (v <= bestVal) continue;
			double f = STARTF + hzPerBin * i;
			double qf = (double)(long)(f / quant_hz + 0.5) * quant_hz;
			bool excluded = false;
			for (int s = 0; s < npk; s++) {
				if (abs(i - selBin[s]) < mindistBins) { excluded = true; break; }
				if (qf == out[s].freqHz) { excluded = true; break; }
			}
			if (excluded) continue;
			bestVal = v; bestBin = i;
		}
		if (bestBin < 0) break;
		double f = STARTF + hzPerBin * bestBin;
		out[npk].freqHz = (double)(long)(f / quant_hz + 0.5) * quant_hz;
		out[npk].power = scanresult[bestBin];
		selBin[npk] = bestBin;
		npk++;
	}
	return npk;
}
#undef SCAN_VALID

Scanner scanner = Scanner();

void Scanner::scanForWeb()
{
	// scan() fills scanresult[]/scandisp[] and peakf; it does NOT draw to the display.
	scan();
	scanWebMillis = millis();
	scanWebSeq = scanWebSeq + 1;	// avoid deprecated ++ on a volatile (C++20 -Wvolatile)
}

uint32_t Scanner::webSeq() { return scanWebSeq; }
uint32_t Scanner::webMillis() { return scanWebMillis; }
int Scanner::dispW() { return scanconfig.PLOT_W; }
double Scanner::stepMHz() { return scanconfig.CHANSTEP * scanconfig.SMPL_PIX / 1000.0; }
const int *Scanner::dispData() { return scandisp; }
double Scanner::peakMHz() { return peakf * 1e-6; }
