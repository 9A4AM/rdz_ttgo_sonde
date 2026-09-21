#include "../features.h"
#if FEATURE_SERIALOUT

#include "conn-serialout.h"
#include "posinfo.h"
#include <time.h>
#include <math.h>
#include <string.h>

extern const char *sondeTypeStr[];
extern const char *sondeTypeStrSH[];

// sondeTypeStr[] pads some names with a trailing space ("DFM ", "M10 "); strip it for data fields.
static void sondeTypeName(SondeInfo *si, char *out, int len) {
	snprintf(out, len, "%s", sondeTypeStr[sonde.realType(si)]);
	for (int i = (int)strlen(out) - 1; i >= 0 && out[i] == ' '; i--) out[i] = 0;
}

// --- validation ------------------------------------------------------------

static bool pinInUse(int pin) {
	// pins already assigned to other peripherals in the config (ignore disabled -1 slots).
	// Prevents remapping Serial1 TX onto the radio/display/SD/GPS/LED bus and killing them.
	const int used[] = {
		sonde.config.sx1278_ss, sonde.config.sx1278_miso,
		sonde.config.sx1278_mosi, sonde.config.sx1278_sck,
		sonde.config.oled_sda, sonde.config.oled_scl, sonde.config.oled_rst,
		sonde.config.tft_rs, sonde.config.tft_cs,
		sonde.config.gps_rxd, sonde.config.gps_txd,
		sonde.config.sd.cs, sonde.config.sd.miso, sonde.config.sd.mosi, sonde.config.sd.clk,
		sonde.config.batt_adc, sonde.config.led_pout,
	};
	for (unsigned i = 0; i < sizeof(used) / sizeof(used[0]); i++)
		if (used[i] >= 0 && used[i] == pin) return true;
	// power/button pins may carry a +128 (0x80) flag (e.g. AXP/touch) or 255=none; compare low 7 bits.
	const int flagged[] = {
		sonde.config.power_pout, sonde.config.button_pin, sonde.config.button2_pin,
	};
	for (unsigned i = 0; i < sizeof(flagged) / sizeof(flagged[0]); i++)
		if (flagged[i] >= 0 && (flagged[i] & 0x7f) == pin) return true;
	return false;
}

static bool validOutputPin(int pin) {
	// must be a real, output-capable GPIO free for our use.
	if (pin < 0) return false;                // -1 = disabled
	if (pin >= 6 && pin <= 11) return false;  // SPI flash pins -> remapping them bootloops the ESP32
	if (pin >= 34 && pin <= 39) return false; // input-only pins (no output)
	if (pinInUse(pin)) return false;          // collides with radio/display/SD/GPS
	return true;
}

bool ConnSerialOut::isEnabled() {
	int f = sonde.config.serialout.format;
	if (f < 1 || f > 4) return false;
	if (!validOutputPin(sonde.config.serialout.txd)) return false;
	if (sonde.config.serialout.baud <= 0) return false;
	return true;
}

// --- formatters ------------------------------------------------------------

static void jsonSonde(SondeInfo *si, char *buf, int len) {
	char tp[6];
	sondeTypeName(si, tp, sizeof(tp));
	snprintf(buf, len,
		"{\"src\":\"sonde\",\"ser\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,\"alt\":%.1f,"
		"\"spd\":%.1f,\"dir\":%.1f,\"sats\":%d,\"time\":%u,\"type\":\"%s\"}\r\n",
		si->d.ser, si->d.lat, si->d.lon, si->d.alt,
		si->d.hs, si->d.dir, si->d.sats, (unsigned)si->d.time,
		tp);
}

static void jsonStation(char *buf, int len) {
	snprintf(buf, len,
		"{\"src\":\"station\",\"lat\":%.5f,\"lon\":%.5f,\"alt\":%d,"
		"\"spd\":%.1f,\"dir\":%d,\"chase\":%d}\r\n",
		posInfo.lat, posInfo.lon, posInfo.alt,
		posInfo.speed, posInfo.course, posInfo.chase);
}

static void csvSonde(SondeInfo *si, char *buf, int len) {
	// src,ser,lat,lon,alt,spd,dir,sats,time,type
	char tp[6];
	sondeTypeName(si, tp, sizeof(tp));
	snprintf(buf, len, "sonde,%s,%.5f,%.5f,%.1f,%.1f,%.1f,%d,%u,%s\r\n",
		si->d.ser, si->d.lat, si->d.lon, si->d.alt, si->d.hs, si->d.dir,
		si->d.sats, (unsigned)si->d.time, tp);
}

static void csvStation(char *buf, int len) {
	// src,ser,lat,lon,alt,spd,dir,sats,time,type  (ser/sats/time/type empty for station)
	snprintf(buf, len, "station,,%.5f,%.5f,%d,%.1f,%d,,,\r\n",
		posInfo.lat, posInfo.lon, posInfo.alt, posInfo.speed, posInfo.course);
}

static uint8_t nmeaChecksum(const char *s) {
	// XOR of all chars between '$' and '*' (s must already exclude them)
	uint8_t c = 0;
	while (*s) c ^= (uint8_t)*s++;
	return c;
}

static void degToNMEA(double deg, bool isLat, char *out, int len, char *hemi) {
	*hemi = isLat ? (deg < 0 ? 'S' : 'N') : (deg < 0 ? 'W' : 'E');
	double a = fabs(deg);
	int d = (int)a;
	double m = (a - d) * 60.0;
	m = round(m * 10000.0) / 10000.0;   // match %.4f precision to avoid printing 60.0000
	if (m >= 60.0) { m -= 60.0; d += 1; }
	if (isLat) snprintf(out, len, "%02d%07.4f", d, m);   // ddmm.mmmm
	else       snprintf(out, len, "%03d%07.4f", d, m);   // dddmm.mmmm
}

// Builds "$<talker>GGA,...*CS\r\n$<talker>RMC,...*CS\r\n" into buf.
// When !validTime the time/date fields are emitted empty (valid NMEA) instead of a bogus 1970 value.
static void nmeaPair(const char *talker, double lat, double lon, double alt,
                     double spd_ms, double course, int sats, uint32_t utime,
                     bool validTime, char *buf, int len) {
	char lats[16], lons[16], lath, lonh;
	degToNMEA(lat, true, lats, sizeof(lats), &lath);
	degToNMEA(lon, false, lons, sizeof(lons), &lonh);
	double knots = spd_ms * 1.9438445;

	char tstr[12], dstr[8];   // hhmmss.00 time, ddmmyy date (empty when time is not valid)
	if (validTime) {
		struct tm tmv;
		time_t t = (time_t)utime;
		gmtime_r(&t, &tmv);
		snprintf(tstr, sizeof(tstr), "%02d%02d%02d.00", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
		snprintf(dstr, sizeof(dstr), "%02d%02d%02d", tmv.tm_mday, tmv.tm_mon + 1, tmv.tm_year % 100);
	} else {
		tstr[0] = 0;
		dstr[0] = 0;
	}

	char gga[110], rmc[110];
	snprintf(gga, sizeof(gga),
		"%sGGA,%s,%s,%c,%s,%c,1,%02d,1.0,%.1f,M,0.0,M,,",
		talker, tstr, lats, lath, lons, lonh, sats, alt);
	snprintf(rmc, sizeof(rmc),
		"%sRMC,%s,A,%s,%c,%s,%c,%.1f,%.1f,%s,,,A",
		talker, tstr, lats, lath, lons, lonh, knots, course, dstr);
	snprintf(buf, len, "$%s*%02X\r\n$%s*%02X\r\n",
		gga, nmeaChecksum(gga), rmc, nmeaChecksum(rmc));
}

static void nmeaSonde(SondeInfo *si, char *buf, int len) {
	nmeaPair("GP", si->d.lat, si->d.lon, si->d.alt, si->d.hs, si->d.dir,
		si->d.sats, si->d.time, si->d.validTime, buf, len);
}

static void nmeaStation(char *buf, int len) {
	// station has no per-fix unix time; use current system time when the clock looks set
	time_t now = time(NULL);
	bool tvalid = now > 1600000000;   // ~2020-09; guards against unset (1970) system clock
	nmeaPair("GN", posInfo.lat, posInfo.lon, posInfo.alt, posInfo.speed,
		posInfo.course, posInfo.sat, (uint32_t)now, tvalid, buf, len);
}

static void payloadSummary(SondeInfo *si, char *buf, int len) {
	char tstr[12];   // "HH:MM:SS", empty when time is not valid
	if (si->d.validTime) {
		struct tm tmv;
		time_t t = (time_t)si->d.time;
		gmtime_r(&t, &tmv);
		snprintf(tstr, sizeof(tstr), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
	} else {
		tstr[0] = 0;
	}
	// Chasemapper convention: RS41 uses the bare serial; other types are prefixed "<TYPE>-<ser>"
	// so downstream can disambiguate the sonde type from the callsign (model still carries the type).
	uint8_t rt = sonde.realType(si);
	char callsign[24];
	if (rt == STYPE_RS41) snprintf(callsign, sizeof(callsign), "%s", si->d.ser);
	else                  snprintf(callsign, sizeof(callsign), "%s-%s", sondeTypeStrSH[rt], si->d.ser);
	snprintf(buf, len,
		"{\"type\":\"PAYLOAD_SUMMARY\",\"callsign\":\"%s\",\"latitude\":%.5f,"
		"\"longitude\":%.5f,\"altitude\":%d,\"speed\":%d,\"heading\":%d,"
		"\"time\":\"%s\",\"model\":\"%s\",\"freq\":\"%.3f MHz\"}\r\n",
		callsign, si->d.lat, si->d.lon, (int)si->d.alt,
		(int)(si->d.hs * 1.9438445), (int)si->d.dir,
		tstr, sondeTypeStrSH[rt], si->freq);
}

// --- Conn interface --------------------------------------------------------

void ConnSerialOut::init() {
	started = isEnabled();
	if (started) {
		begunTxd = sonde.config.serialout.txd;
		begunBaud = sonde.config.serialout.baud;
		// RX disabled (-1), TX on configured pin
		Serial1.begin(begunBaud, SERIAL_8N1, -1, begunTxd);
		Serial.printf("SerialOut: enabled, format=%d pin=%d baud=%d\n",
			sonde.config.serialout.format, begunTxd, begunBaud);
	}
}

void ConnSerialOut::netsetup() {}
void ConnSerialOut::netshutdown() {}

void ConnSerialOut::updateSonde(SondeInfo *si) {
	if (!started) return;
	char line[320];
	line[0] = 0;
	switch (sonde.config.serialout.format) {
	case 1: nmeaSonde(si, line, sizeof(line)); break;
	case 2: jsonSonde(si, line, sizeof(line)); break;
	case 3: csvSonde(si, line, sizeof(line)); break;
	case 4: payloadSummary(si, line, sizeof(line)); break;
	default: return;
	}
	// Serial1.print is blocking; at low baud a full line adds a few ms to loop(). Bounded here
	// by the decoded-frame rate (~1/s), so no throttle needed on the sonde path.
	if (line[0]) Serial1.print(line);
}

void ConnSerialOut::updateStation(PosInfo *pi) {
	if (!started) return;
	if (!posInfo.valid) return;
	// Throttle to ~1 Hz: updateStation() is called every loopDecoder pass (can be >1 Hz in
	// scan/no-signal cycles); this also bounds the blocking Serial1 write below to one line/sec.
	unsigned long now = millis();
	if (lastStationMs != 0 && (now - lastStationMs) < 1000) return;
	lastStationMs = now;
	char line[320];
	line[0] = 0;
	switch (sonde.config.serialout.format) {
	case 1: nmeaStation(line, sizeof(line)); break;
	case 2: jsonStation(line, sizeof(line)); break;
	case 3: csvStation(line, sizeof(line)); break;
	default: return; // format 4 (PAYLOAD_SUMMARY) intentionally emits nothing for the station: payload-only concept
	}
	if (line[0]) Serial1.print(line);
}

String ConnSerialOut::getStatus() {
	int f = sonde.config.serialout.format;
	if (f < 1 || f > 4) return String("disabled: off/invalid format");
	if (!validOutputPin(sonde.config.serialout.txd)) return String("disabled: invalid/conflicting pin");
	if (sonde.config.serialout.baud <= 0) return String("disabled: invalid baud");
	const char *fmt = f == 1 ? "NMEA" : f == 2 ? "JSON" : f == 3 ? "CSV" : "PAYLOAD_SUMMARY";
	if (!started) {
		// config now looks valid, but Serial1 was not opened at boot with it
		return String("configured, reboot to apply");
	}
	// report the pin/baud Serial1 is actually running on (latched at boot; changes need a reboot)
	char info[80];
	snprintf(info, sizeof(info), "active [%s, pin %d, %d baud]", fmt, begunTxd, begunBaud);
	return String(info);
}

String ConnSerialOut::getName() {
	return String("SerialOut");
}

ConnSerialOut connSerialOut;
#endif
