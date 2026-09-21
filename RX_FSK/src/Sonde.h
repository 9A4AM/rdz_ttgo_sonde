
#ifndef Sonde_h
#define Sonde_h


#include <inttypes.h>
#include <Arduino.h>

typedef enum BoardTypes {
   BOARD_TBEAM_0_7,
   BOARD_TBEAM_1_0,
   BOARD_TBEAM_1_1,
   BOARD_TBEAM_1_2,
   BOARD_TTGO_LORA_2,
   BOARD_TTGOv1_HELTEC,
   BOARD_M5_CORE_GRAY,
   BOARD_M5_CORE2,
   BOARD_CYD_E32R28T,
   BOARD_UNKNOWN
} BoardTypes;



// RX_TIMEOUT: no header detected
// RX_ERROR: header detected, but data not decoded (crc error, etc.)
// RX_PARTIAL: header detected, some data ok, some with errors
//	For RS41: Some blocks with CRC error, some blocks ok in a single frame
//      For DFM: In +- 1s, some but not all DAT-subframes 1,2,3,4,5,6,7,8 received
//      For RS92 ??? unclear
//      For M10/M20 its always all or nothing, no PARTIAL data 
//      For MP3H its alway all or nothing, no PARTIAL data
// RX_OK: header and all data ok
enum RxResult { RX_OK, RX_TIMEOUT, RX_ERROR, RX_UNKNOWN, RX_NOPOS };
#define RX_UPDATERSSI 0xFFFE

// Events that change what is displayed (mode, sondenr)
// Keys:
// 1  Button (short)  or Touch (short)
// 2  Button (double) or Touch (double)
// 3  Button (mid)    or Touch (mid)
// 4  Button (long)   or Touch (long)
// 5  Touch1/2 (short)
// 6  Touch1/2 (double)
// 7  Touch1/2 (mid)
// 8  Touch1/2 (long)

/* Keypress => Sonde++ / Sonde-- / Display:=N*/
enum Events { EVT_NONE, EVT_KEY1SHORT, EVT_KEY1DOUBLE, EVT_KEY1MID, EVT_KEY1LONG,
                        EVT_KEY2SHORT, EVT_KEY2DOUBLE, EVT_KEY2MID, EVT_KEY2LONG,
                        EVT_VIEWTO, EVT_RXTO, EVT_NORXTO,
			EVT_RINEX, EVT_FORMAT,
              EVT_MAX };
extern const char *evstring[];
extern const char *RXstr[];
#define EVENTNAME(s) evstring[s]

//int8_t actions[EVT_MAX];
#define ACT_NONE 255
#define ACT_DISPLAY(n) (n)
#define ACT_MAXDISPLAY 50
#define ACT_DISPLAY_SCANNER 0
#define ACT_DISPLAY_NEXT 64
#define ACT_DISPLAY_DEFAULT 63
#define ACT_DISPLAY_SPECTRUM 62
#define ACT_DISPLAY_WIFI 61
#define ACT_NEXTSONDE 65
#define ACT_PREVSONDE 66
#define ACT_ADDFREQ(n) ((n)+64)
#define ACT_SONDE(n) ((n)+128)
#define ACT_RINEX_UPDATE 60
#define ACT_FORMAT_SD 59

#define ACT_IS_FREQ_CHANGE(n) (((n)>=65)&&((n)<255))

// 0000nnnn => goto display nnnn
// 01000000 => goto sonde -1
// 01000001 => goto sonde +1

#define NSondeTypes 7
enum SondeType { STYPE_DFM, STYPE_RS41, STYPE_RS92, STYPE_M10M20, STYPE_M10, STYPE_M20, STYPE_MP3H };
extern const char *sondeTypeStr[NSondeTypes];
extern const char *sondeTypeLongStr[NSondeTypes];
extern const char sondeTypeChar[NSondeTypes];
extern const char *manufacturer_string[NSondeTypes];

#define ISOLED(cfg) ((cfg).disptype==0 || (cfg).disptype==2)

#define TYPE_IS_DFM(t) ( (t)==STYPE_DFM )
#define TYPE_IS_METEO(t) ( (t)==STYPE_M10M20 || (t)==STYPE_M10 || (t)==STYPE_M20 )

#define VALIDPOS(x) (((x)&0x03)==0x03)
#define VALIDALT(x) ((x)&0x04)
#define VALIDVS(x) ((x)&0x08)
#define VALIDHS(x) ((x)&0x10)
#define VALIDDIR(x) ((x)&0x20)
#define VALIDSATS(x) ((x)&0x40)

typedef struct st_sondedata {
        // decoded ID
        char id[10];
	char ser[12];
        bool validID;
	char typestr[5];			// decoded type (use type if *typestr==0)
	int8_t subtype;   /* 0 for none/unknown, hex type for dfm, 1/2 for M10/M20 */
        // decoded position
        float lat;			// latitude
        float lon;			// longitude
        float alt;			// altitude
        float vs;			// vertical speed in m/s
        float hs;			// horizontal speed in m/s
	float dir; 			// 0..360
	uint8_t sats;			// number of sats
        uint8_t validPos;   // bit pattern for validity of above 7 fields; 0x80: position is old
	// decoded GPS time
	uint32_t time;
	uint32_t frame;
	uint32_t vframe;		// vframe==frame if frame is unique/continous, otherweise vframe is derived from gps time
	bool validTime;
	// shut down timers, currently only for RS41; -1=disabled
	uint16_t launchKT, burstKT, countKT;
	uint16_t crefKT; // frame number in which countKT was last sent
	// sonde specific extra data, NULL if unused or not yet initialized, currently used for RS41 subframe data (calibration)
	float temperature; // platinum resistor temperature
	float tempRHSensor; // temperature of relative humidity sensor
	float relativeHumidity; // relative humidity
	float pressure;
	float batteryVoltage = -1;
} SondeData;

typedef struct st_sondeinfo {
	// First part: static configuration, not decoded data. 
        // receiver configuration
	bool active;
        SondeType type;
        float freq;
	char launchsite[18];		

	// Second part: internal decoder state. no need to clear this on new sonde
        // RSSI from receiver
        int rssi;			// signal strength
	int32_t afc;			// afc correction value
	// statistics
	uint8_t rxStat[20];
	uint32_t rxStart;    		// millis() timestamp of continuous rx start
	uint32_t rxtime;    		// wall-clock (time()) seconds when this frame was received; for upload cache replay
	uint32_t norxStart;		// millis() timestamp of continuous no rx start
	uint32_t viewStart;		// millis() timestamp of viewinf this sonde with current display
	int8_t lastState;		// -1: disabled; 0: norx; 1: rx

	// Third part: decoded data. Clear if reception of a new sonde has started
	SondeData d;

	// Decoder-specific data, dynamically allocated (for RS41: calibration data)
        void *extra;
} SondeInfo;
// rxStat: 3=undef[empty] 1=timeout[.] 2=errro[E] 0=ok[|] 4=no valid position[°]

// Used for interacting with the RX background task
typedef struct st_RXTask {
	// Variables set by Arduino main loop to value >=0 for requesting
	// mode change to sonde reception for sonde <value) in RXTask.
	// Will be reset to -1 by RXTask
	int activate;
	// Variables set by RXTask, corresponding to mode ST_DECODER (if active) or something else,
	// and currently received sonde
	int mainState;
	int currentSonde;
	// Variable set by RXTask to communicate status to Arduino task
	// via waitRXcomplete function
	uint16_t receiveResult;
	uint16_t receiveSonde;  // sonde inde corresponding to receiveResult
	// status variabe set by decoder to indicate something is broken
	// int fifoOverflow;
} RXTask;

extern RXTask rxtask;

struct st_rs41config {
	int agcbw;
	int rxbw;
};
struct st_rs92config {
	int rxbw;
	int alt2d;
};
struct st_dfmconfig {
	int agcbw;
	int rxbw;
};
struct st_m10m20config {
	int agcbw;
	int rxbw;
};
struct st_mp3hconfig {
	int agcbw;
	int rxbw;
};

struct st_axudpinfo {
        int active;
        char host[64];
        int ratelimit;
};

struct st_aprsinfo {
        int active;
        char host[64];
        char host2[64];
        char symbol[3];
        int lowrate;
        int highrate;
        int lowlimit;
        int timeout;
};

// maybe extend for external Bluetooth interface?
// internal bluetooth consumes too much memory
struct st_kisstnc {
        bool active;
};

struct st_mqtt {
	int active;
	char id[64];
	char host[64];
	int port;
	char username[64];
	char password[64];
	char prefix[64];
	int report_interval;
};

struct st_cm {
	int active;
	char host[64];
	int port;
};

struct st_ss {
 	int active;
 	char host[64];
 	int port;
 };

struct st_notify {
	int  active;        // alert bitfield: 0=off, 1=landing near me, 2=new sonde, 3=both
	int  dist;          // horizontal distance threshold, km (landing alert)
	int  alt;           // altitude threshold, km (landing alert only below this)
	char server[96];    // ntfy server base URL, http only (e.g. http://ntfy.sh; no TLS in this fw)
	char topic[48];     // ntfy topic (publish target = server + "/" + topic)
	char token[64];     // optional bearer token ("" = none)
};

struct st_sondehub {
	int active;
	int chase;
	char host[64];
	char callsign[64];
	char antenna[64];
	char email[64];
        int fiactive;
	int fiinterval;
	int fimaxdist;
	double fimaxage;
};

struct st_sdcard {
	int cs;
	int miso;
	int mosi;
	int clk;
	int sync;
	int name;
	int speed;	/* SPI speed in Hz (0 = library default), e.g. 4000000, 8000000, 16000000 */
};

struct st_serialout {
	int format;   // 0=off, 1=NMEA, 2=JSON, 3=CSV, 4=PAYLOAD_SUMMARY
	int txd;      // TX pin (GPIO); -1 = disabled
	int baud;     // baud rate, e.g. 9600
};

// to be extended
enum { TYPE_TTGO, TYPE_M5_CORE2, TYPE_M5_CORE };

typedef struct st_rdzconfig {
	int type;			// autodetected type, TTGO or M5_CORE2
	int cachesize;   // offline upload cache: number of frames to buffer (0 = disabled)
	// hardware configuration
	int button_pin;			// PIN port number menu button (+128 for touch mode)
	int button2_pin;		// PIN port number menu button (+128 for touch mode)
	int button2_axp;		// Use AXP192 power button as button2
	int touch_thresh;		// Threshold value (0..100) for touch input button
	int led_pout;			// POUT port number of LED (used as serial monitor)
	int power_pout;			// Power control pin (for Heltec v2)
	int disptype;			// 0=OLED; 1=ILI9225
	int oled_sda;			// OLED/TFT data pin 
	int oled_scl;			// OLED/TFT clock pin
	int oled_rst;			// OLED/TFT reset pin
	int tft_rs;			// TFT RS pin
	int tft_cs;			// TFT CS pin
	int tft_orient;			// TFT orientation (default: 1)
	int tft_spifreq;		// SPI transfer speed (default 40M is out of spec for some TFT)
	int gps_rxd;			// GPS module RXD pin. We expect 9600 baud NMEA data.
	int gps_txd;			// GPS module TXD pin
	int batt_adc;			// Pin for ADC battery measurement (GPIO35 on TTGO V2.1_1.6)
	int sx1278_ss;			// SPI slave select for sx1278
	int sx1278_miso;		// SPI MISO for sx1278
	int sx1278_mosi;		// SPI MOSI for sx1278
	int sx1278_sck;			// SPI SCK for sx1278
	// software configuration
	int debug;				// show port and config options after reboot
	double rxlat;
	double rxlon;
	double rxalt;
	int wifi;				// connect to known WLAN 0=skip
        uint16_t b2mute;			// mute LED/Buzzer for x minutes, 0=0ff
	int screenfile;
	int8_t display[30];			// list of display mode (0:scanner, 1:default, 2,... additional modes)
	int dispsaver;			// Turn display on/off (0=always on, 10*n+1: off after n seconds, 
					//	10*n+2: scanner off after n seconds, RX always shown)
	int dispcontrast;		// For OLED: set contrast to 0..255 (-1: don't set/leave at factory default)
	double startfreq;		// spectrum/scan start freq in MHz (decimals allowed, e.g. 400.2); sweep covers startfreq..startfreq+~6 MHz
	int channelbw;			// spectrum channel bandwidth (valid: 5, 10, 20, 25, 50, 100 kHz)	
	int spectrum;			// show freq spectrum for n seconds -1=disable; 0=forever
	int marker;				// show freq marker in spectrum  0=disable
	int maxsonde;			// number of max sonde in scan (range=1-99)
	int norx_timeout;		// Time after which rx mode switches to scan mode (without rx signal)
	int noisefloor;			// for spectrum display
	int scanplotint;		// web scan-plot idle-sweep interval in seconds (0=disable)
	// Spectrum-sweep per-bin RSSI dwell overrides (affect both the spectrum view
	// and auto-scan; -1 = use the per-display default). Longer dwell = steadier
	// RSSI / better weak-peak detection, slower sweep. Plot geometry is unaffected.
	int scan_smooth;        // SX1278 RSSI averaging exponent 0..7 (samples = 2^(n+1)); -1 = default
	int scan_addwait;       // extra per-bin settle time in microseconds; -1 = default
	int scan_iter;          // number of full sweeps per scan (max RSSI kept); 1..20, default 6
	// Auto-scan (peak detection) settings. When autoscan_enable is set, the
	// firmware ignores the configured channel list and instead sweeps the
	// spectrum, finds peaks above the noise floor and trial-decodes each one,
	// like radiosonde_auto_rx. Names/defaults mirror auto_rx's scanner.
	int autoscan_enable;    // 1 = use peak-detection auto-scan instead of the channel list
	int autoscan_snr;       // min SNR (dB) above the (median) noise floor for a peak
	int autoscan_mindist;   // min distance between detected peaks (Hz)
	int autoscan_quant;     // quantize detected peaks to this step (Hz; sondes use 10 kHz)
	int autoscan_maxpeaks;  // max peaks to trial-decode per sweep
	int autoscan_dwell;     // per-peak detection budget (s); per-type = this/N_types unless typedwell set
	int autoscan_typedwell; // fixed decode time per sonde type (ms); 0 = derive from autoscan_dwell
	int autoscan_qrgfirst;  // 1 = each cycle try the active channel-list QRGs (configured freq+type) before the spectrum peaks
	char autoscan_exclude[64]; // comma-separated MHz of known noise/birdies to ignore in peak detection (e.g. "400.01,400.11")
	int allowfileupload;		// allow firmware/filesystem upload from the update page (0=disable). Hidden: not in cfg.js
	char mdnsname[15];		// mDNS-Name, defaults to rdzsonde
	// receiver configuration
	int freqofs;			// frequency offset (tuner config = rx frequency + freqofs) in Hz
	int lnaboost;			// LnaBoostHf (RegLna 0x0C bits 1-0): 0=default LNA current, 1=boost on (150%)
	int lnagain;			// external LNA gain in dB (>=0), subtracted from reported RSSI (0 = no correction)
	struct st_rs41config rs41;	// configuration options specific for RS41 receiver
	struct st_rs92config rs92;
	struct st_dfmconfig dfm;
	struct st_m10m20config m10m20;
	struct st_mp3hconfig mp3h;
	char ephftp[80];
	// data feed configuration
	// for now, one feed for each type is enough, but might get extended to more?
	char call[10];			// APRS callsign
	int passcode;		// APRS passcode
	int chase;
	char objcall[10];		// APRS object call (for wettersonde.net)
	char beaconsym[5];		// APRS beacon symbol
	char comment[32];
	struct st_axudpinfo udpfeed;	// target for AXUDP messages
	struct st_aprsinfo tcpfeed;	// target for APRS-IS TCP connections
	struct st_kisstnc kisstnc;	// target for KISS TNC (via TCP, mainly for APRSdroid)
	struct st_mqtt mqtt;
	struct st_sondehub sondehub;
	struct st_cm cm;
	struct st_sdcard sd;
	struct st_ss ss;
	struct st_notify notify;
	struct st_serialout serialout;	// local serial position output (Serial1)
} RDZConfig;


struct st_configitems {
  const char *name;
  //  const char *label; => now handled in JS
  int type;  // 0: numeric; i>0 string of length i; -1: separator; -2: type selector
  void *data;
};

// defined in RX_FSK.ino
extern struct st_configitems config_list[];
extern const int N_CONFIG;


#define MAXSONDE 50

extern int fingerprintValue[];
extern const char *fingerprintText[];

enum BoardTypes;

class Sonde
{
private:
	void setDefaultConfig(BoardTypes bt);
public:
	RDZConfig config;
	int fingerprint = 0;
	int currentSonde = 0;
	int nSonde;
	String ipaddr;
	bool isAP;
	// moved to heap, saving space in .bss
	//SondeInfo sondeList[MAXSONDE+1];
	SondeInfo *sondeList;
	// helper function for type string
	static SondeType realType(SondeInfo *si);

	Sonde();
	void defaultConfig();
	void checkConfig();
	void setConfig(const char *str);

	void clearSonde();
	void addSonde(float frequency, SondeType type, int active, char *launchsite);
	void nextConfig();
	void nextRxSonde();
	void nextRxFreq(int addkhz);

	/* new interface */
	void setup();
	void receive();
	// Tune+decode one frame for the current sondeList[rxtask.currentSonde] entry,
	// returning the raw decoder result (RX_OK/RX_ERROR/RX_TIMEOUT). Used by the
	// auto-scan trial-decode loop; unlike receive() it has no event/timeout/display
	// side effects.
	uint16_t rxRawFrame();
	uint16_t waitRXcomplete();

	SondeInfo *si();
	void clearAllData(SondeInfo *si);

	uint8_t timeoutEvent(SondeInfo *si);
	uint8_t updateState(uint8_t event);

	void updateDisplayPos();
	void updateDisplayPos2();
	void updateDisplayID();
	void updateDisplayRSSI();
	void updateDisplayRXConfig();
	void updateStat();
	void updateDisplayIP();
	void updateDisplay();
	void clearDisplay();
        void dispsavectlON();
        void dispsavectlOFF(int rxactive);

	void setIP(String ip, bool isAP);
};

extern Sonde sonde;

#endif

