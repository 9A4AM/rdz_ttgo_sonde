#include "features.h"
#include "version.h"
#include "core.h"

#define TAG "RX_FSK"
#include "src/logger.h"

#include <dirent.h>

#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPAsyncWebServer.h>

#include <LittleFS.h>

#include <SPI.h>
#include <Update.h>
#include <ESPmDNS.h>
#include <Ticker.h>
#include "esp_heap_caps.h"
#if FEATURE_NOTIFY
#include <esp_random.h>
#endif
//#include <rtc_wdt.h>
//#include "soc/timer_group_struct.h"
//#include "soc/timer_group_reg.h"

#include "src/SX1278FSK.h"
#include "src/Sonde.h"
#include "src/Display.h"
#include "src/Scanner.h"
#if FEATURE_RS92
#include "src/geteph.h"
#include "src/rs92gps.h"
#endif

#include "src/ShFreqImport.h"
#include "src/RS41.h"
#include "src/DFM.h"
#include "src/json.h"
#include "src/posinfo.h"

#include "src/pmu.h"
#include "src/user.h"
#include "src/crypto.h"


/* Data exchange connectors */
#if FEATURE_SONDESEEKER
#include "src/conn-sondeseeker.h"
#endif
#if FEATURE_CHASEMAPPER
#include "src/conn-chasemapper.h"
#endif
#if FEATURE_MQTT
#include "src/conn-mqtt.h"
#endif
#if FEATURE_NOTIFY
#include "src/conn-notify.h"
#endif
#if FEATURE_SDCARD
#include "src/conn-sdcard.h"
#endif
#if FEATURE_SERIALOUT
#include "src/conn-serialout.h"
#endif
#if FEATURE_APRS
#include "src/conn-aprs.h"
#endif
#if FEATURE_SONDEHUB
#include "src/conn-sondehub.h"
#endif

#include "src/conn-system.h"
#include "src/conn-cache.h"

extern SemaphoreHandle_t globalLock;

Conn *connectors[] = { &connSystem,
&connGPS,
#if FEATURE_APRS
&connAPRS,
#endif
#if FEATURE_SONDEHUB
&connSondehub,
#endif
#if FEATURE_CHASEMAPPER
&connChasemapper,
#endif
#if FEATURE_SONDESEEKER
&connSondeseeker,
#endif
#if FEATURE_MQTT
&connMQTT,
#endif
#if FEATURE_NOTIFY
&connNotify,
#endif
#if FEATURE_SDCARD
&connSDCard,
#endif
#if FEATURE_SERIALOUT
&connSerialOut,
#endif
NULL };

//#define ESP_MEM_DEBUG 1
//int e;

enum MainState { ST_DECODER, ST_SPECTRUM, ST_WIFISCAN, ST_UPDATE, ST_TOUCHCALIB, ST_RINEX_UPDATE, ST_FORMAT_SD, ST_AUTOSCAN };
static MainState mainState = ST_WIFISCAN;
const char *mainStateStr[] = {"DECODER", "SPECTRUM", "WIFISCAN", "UPDATE", "TOUCHCALIB", "RINEXUPDATE", "FORMATSD", "AUTOSCAN" };

AsyncWebServer server(80);

PMU *pmu = NULL;
SemaphoreHandle_t axpSemaphore;
extern uint8_t pmu_irq;

// Selectable update servers (the download host lives here, not in the browser).
// The web update page (upd.html) picks one via the POST button name; handleUpdatePost
// sets updateHost/updatePort/updatePrefix accordingly before entering ST_UPDATE.
const char *updateHostOfficial = "rdzsonde.org";        // main/dev2 branches
const char *updateHostPy5ol   = "rdzttgo.nano.dev.br"; // single flat build (prefix "/")
const char *updateHost = updateHostOfficial;            // active host for execOTA()
int updatePort = 80;

const char *updatePrefixM = "/main/";
const char *updatePrefixD = "/dev2/";
const char *updatePrefixP = "/";                        // py5ol flat layout
const char *updatePrefix = updatePrefixM;
const char *updateFs = "update.fs.bin";
const char *updateIno = "update.ino.bin";

#define LOCALUDPPORT 9002
//Get real UTC time from NTP server
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0; //UTC
const int   daylightOffset_sec = 0; //UTC

// Authentication management
// BootID is embedded in index.html so client can invalidate auth cookie after ttgo reboot
// defaultUserLevel: anonymous (not-logged-in) access level, derived from user.txt --
// full access until the first user is registered, then no access (see getDefaultAuthLevel)
char bootid[8];
uint8_t defaultUserLevel = 2;

boolean connected = false;
WiFiUDP udp;
WiFiClient client;

/* Sonde.h: enum SondeType { STYPE_DFM,, STYPE_RS41, STYPE_RS92, STYPE_M10M20, STYPE_M10, STYPE_M20, STYPE_MP3H }; */
const char *sondeTypeStrSH[NSondeTypes] = { "DFM", "RS41", "RS92", "Mxx"/*never sent*/, "M10", "M20", "MRZ" };


// moved to connSondehub.cpp
//#if FEATURE_SONDEHUB
//#define SONDEHUB_STATION_UPDATE_TIME (60*60*1000) // 60 min
//#define SONDEHUB_MOBILE_STATION_UPDATE_TIME (30*1000) // 30 sec
//WiFiClient shclient;	// Sondehub v2
//int shImportInterval = 0;
//char shImport = 0;
//unsigned long time_last_update = 0;
//#endif

// JSON over TCP for communicating with the rdzSonde (rdzwx-go) Android app
WiFiServer rdzserver(14570);
WiFiClient rdzclient;

// If a file "localupd.txt" exists, firmware can be updated from a custom IP address read from this file, stored in localUpdates.
// By default (localUpdates==NULL) this is disabled to prevent abuse
// Note: by enabling this, someone with access to the web interface can replace the firmware arbitrarily!
// Make sure that only trustworthy persons have access to the web interface...
char *localUpdates = NULL;

boolean forceReloadScreenConfig = false;

enum KeyPress { KP_NONE = 0, KP_SHORT, KP_DOUBLE, KP_MID, KP_LONG, KP_RINEX, KP_FORMAT };

// "doublepress" is now also used to eliminate key glitch on TTGO T-Beam startup (SENSOR_VN/GPIO39)
struct Button {
  uint8_t pin;
  uint32_t numberKeyPresses;
  KeyPress pressed;
  unsigned long keydownTime;
  int8_t doublepress;
  bool isTouched;
};
Button button1 = {0, 0, KP_NONE, 0, -1, false};
Button button2 = {0, 0, KP_NONE, 0, -1, false};


static int lastDisplay = 1;
static int currentDisplay = 1;

// timestamp when spectrum display was activated
static unsigned long specTimer;

void enterMode(int mode, bool force = false);
void loopAutoScan();
static void autoscanReset();
// Auto-scan is on when configured; it then ignores the channel list entirely.
static inline bool autoscanActive() { return sonde.config.autoscan_enable != 0; }
// Scratch channel slot used by auto-scan for trial/locked decoding. It is the spare
// last entry of sondeList (allocated with MAXSONDE+1 slots), i.e. OUTSIDE the configured
// range [0,maxsonde) -- so it never overwrites a user channel and never shows up in the
// QRG editor or the per-channel status/KML loops. setup() accepts this index specially.
static inline int autoscanSlot() { return MAXSONDE; }
void WiFiEvent(WiFiEvent_t event);


// Possibly we will need more fine grained permissions in the future...
// For now, disallow arbitrary firmware updates on standard installations
// development installations can add a file "localupd.txt" which enables updates from arbitrary locations
int checkAllowed(const char *filename) {
    if(!localUpdates && (strstr(filename, "localupd.txt") != NULL)) return 0;
    return 1;
}

// Files that hold credentials and must not be served to unauthenticated clients.
bool isSensitiveFile(const char *url) {
    static const char *deny[] = { "user.txt", "networks.txt", "config.txt" };
    for(unsigned i=0; i<sizeof(deny)/sizeof(deny[0]); i++) {
        if(strstr(url, deny[i]) != NULL) return true;
    }
    return false;
}

// Web assets (stylesheets, scripts, page templates, map/track overlays, images, fonts) that are
// safe to serve to unauthenticated clients -- the public Home/Data/Livemap/login pages need them.
// Anything else reaching the static fallback (notably the *.txt config/data files and GPSRESET)
// is treated as private and requires a logged-in session.
bool isPublicStaticAsset(const String &url) {
    return url.endsWith(".css")  || url.endsWith(".js")   || url.endsWith(".html") ||
           url.endsWith(".htm")  || url.endsWith(".gpx")  || url.endsWith(".kml")  ||
           url.endsWith(".ico")  || url.endsWith(".png")  || url.endsWith(".jpg")  ||
           url.endsWith(".jpeg") || url.endsWith(".gif")  || url.endsWith(".svg")  ||
           url.endsWith(".woff") || url.endsWith(".woff2")|| url.endsWith(".ttf");
}


// Read line from file, independent of line termination (LF or CR LF)
String readLine(Stream &stream) {
  String s = stream.readStringUntil('\n');
  int len = s.length();
  if (len == 0) return s;
  if (s.charAt(len - 1) == '\r') s.remove(len - 1);
  return s;
}

// Read line from file, without using dynamic memory allocation (String class)
// returns length line.
int readLine(Stream &stream, char *buffer, int maxlen) {
  int n = stream.readBytesUntil('\n', buffer, maxlen);
  buffer[n] = 0;
  if (n <= 0) return 0;
  if (buffer[n - 1] == '\r') {
    buffer[n - 1] = 0;
    n--;
  }
  return n;
}


// Replaces placeholder with LED state value
String processor(const String& var) {
  LOG_D(TAG, "%s\n", var.c_str());
  if (var == "MAPCENTER") {
#if 0
    double lat, lon;
    if (gpsPos.valid) {
      lat = gpsPos.lat;
      lon = gpsPos.lon;
    }
    else {
      lat = sonde.config.rxlat;
      lon = sonde.config.rxlon;
    }
    //if ( !isnan(lat) && !isnan(lon) ) {
#endif
    if ( posInfo.valid ) {
      char p[40];
      snprintf(p, 40, "%.8g,%.8g", posInfo.lat, posInfo.lon);
      return String(p);
    } else {
      return String("48,13");
    }
  }
  if (var == "BOOTID") {
    return String(bootid);
  }
  if (var == "VERSION_NAME") {
    return String(version_name);
  }
  if (var == "VERSION_ID") {
    return String(version_id);
  }
  if (var == "FULLNAMEID") {
    char tmp[128];
    snprintf(tmp, 128, "%s-%c%d", version_id, FS_MAJOR + 'A' - 1, FS_MINOR);
    return String(tmp);
  }
  if (var == "AUTODETECT_INFO") {
    char tmpstr[128];
    const char *fpstr;
    int i = 0;
    while (fingerprintValue[i] != sonde.fingerprint && fingerprintValue[i] != -1) i++;
    if (fingerprintValue[i] == -1) {
      fpstr = "Unknown board";
    } else {
      fpstr = fingerprintText[i];
    }
    snprintf(tmpstr, 128, "Fingerprint %d (%s)", sonde.fingerprint, fpstr);
    return String(tmpstr);
  }
  if (var == "EPHSTATE") {
#if FEATURE_RS92
    return String(ephtxt[ephstate]);
#else
    return String("Not supported");
#endif
  }
  if (var == "LOCAL_UPDATES") {
    if(localUpdates) return String(localUpdates);
    else return String();
  }
  if (var == "ALLOWFILEUPLOAD") {
    return String(sonde.config.allowfileupload);
  }
  return String();
}

const String sondeTypeSelect(int activeType) {
  String sts = "";
  for (int i = 0; i < NSondeTypes; i++) {
    sts += "<option value=\"";
    sts += sondeTypeLongStr[i];
    sts += "\"";
    if (activeType == i) {
      sts += " selected";
    }
    sts += ">";
    sts += sondeTypeLongStr[i];
    sts += "</option>";
  }
  return sts;
}


//trying to work around
//"assertion "heap != NULL && "free() target pointer is outside heap areas"" failed:"
// which happens if request->send is called in createQRGForm!?!??
char message[10240 * 3 - 2048]; //needs to be large enough for all forms (not checked in code)
// QRG form is currently about 24kb with 100 entries

///////////////////////// Functions for Reading / Writing QRG list from/to qrg.txt

void setupChannelList() {
  File file = LittleFS.open("/qrg.txt", "r");
  if (!file) {
    LOG_E(TAG, "There was an error opening the file '/qrg.txt' for reading");
    return;
  }
  int i = 0;
  char launchsite[17] = "                ";
  sonde.clearSonde();
  LOG_I(TAG, "Reading qrg.txt:");
  while (file.available()) {
    String line = readLine(file);
    String sitename;
    if (line[0] == '#') continue;
    char *space = strchr(line.c_str(), ' ');
    if (!space) continue;
    *space = 0;
    float freq = atof(line.c_str());
    SondeType type;
    if (space[1] == '4') {
      type = STYPE_RS41;
    } else if (space[1] == 'R') {
      type = STYPE_RS92;
    }
    else if (space[1] == 'D' || space[1] == '9' || space[1] == '6') {
      type = STYPE_DFM;
    }
    else if (space[1] == 'M') {
      type = STYPE_M10M20;
    }
    else if (space[1] == '2') {
      type = STYPE_M10M20;
    }
    else if (space[1] == '3') {
      type = STYPE_MP3H;
    }
    else continue;
    // active flag (space[3]) and launch site (space[5..]) are optional and only
    // present on longer lines; guard the reads against the real line length so we
    // do not read past the line terminator into the String's reserve capacity.
    int rest = line.length() - (int)(space - line.c_str());
    int active = (rest >= 4 && space[3] == '+') ? 1 : 0;
    if (rest >= 5 && space[4] == ' ') {
      memset(launchsite, ' ', 16);
      strncpy(launchsite, space + 5, 16);
      if (sonde.config.debug == 1) {
        LOG_D(TAG, "Add %f - sondetype: %d (on/off: %d) - site #%d - name: %s\n ", freq, type, active, i, launchsite);
      }
    }
    sonde.addSonde(freq, type, active, launchsite);
    i++;
  }
  file.close();
}

// Emit the standard HTML head into ptr, version-tagging style.css (?v=<version_id>) so browsers
// refetch it whenever the firmware version changes. The static handlers cache assets aggressively
// (max-age) as a network-stack workaround, so the query string is what busts that cache on update.
void HTMLHEAD_V(char *ptr) {
  sprintf(ptr, "<!DOCTYPE html><html><head> <meta charset=\"UTF-8\"> "
               "<meta name=\"color-scheme\" content=\"light dark\"> "
               "<script src=\"theme.js?v=%s\"></script> "
               "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css?v=%s\">", version_id, version_id);
}
void HTMLBODY_OS(char *ptr, const char *which, const char *onsubmit) {
  strcat(ptr, "<body><form class=\"wrapper\" action=\"");
  strcat(ptr, which);
  if(onsubmit) {
     strcat(ptr, "\" onsubmit=\"");
     strcat(ptr, onsubmit);
  }
  strcat(ptr, "\" method=\"post\"><div class=\"content\">");
}
void HTMLBODY(char *ptr, const char *which) { HTMLBODY_OS(ptr, which, NULL); }
void HTMLBODYEND(char *ptr) {
  strcat(ptr, "</div></form></body></html>");
}
// Render the form footer. The "Save changes" submit button is emitted only for level-2
// (admin) viewers; level-1 users may view these forms but cannot save, so hiding the button
// matches the server-side POST guard (which still enforces it regardless).
// footerextra (admin-only) is injected into the footer between the Save button and the version
// info -- used by the qrg/config forms for their backup/restore icon buttons.
void HTMLSAVEBUTTON_F(char *ptr, int level, const char *footerextra) {
  // footer-left groups the Save button with the (optional) backup/restore buttons so they sit
  // next to each other on the left; the version info stays on the right (footer is space-between).
  strcat(ptr, "</div><div class=\"footer\"><div class=\"footer-left\">");
  if(level >= 2)
    strcat(ptr, "<input type=\"submit\" class=\"save\" value=\"Save changes\"/>");
  if(level >= 2 && footerextra)
    strcat(ptr, footerextra);
  strcat(ptr, "</div><span class=\"ttgoinfo\">rdzTTGOserver ");
  strcat(ptr, version_id);
  strcat(ptr, "</span>");
}
void HTMLSAVEBUTTON(char *ptr, int level) { HTMLSAVEBUTTON_F(ptr, level, NULL); }

// Custom inline SVG icons (stroke uses currentColor -> inherits the button's white text colour).
// Backup = "Save" (floppy disk); Restore = "Open" (folder) -- the classic save/open pairing.
#define SVG_BACKUP "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" " \
  "stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">" \
  "<path d=\"M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z\"/>" \
  "<polyline points=\"17 21 17 13 7 13 7 21\"/><polyline points=\"7 3 7 8 15 8\"/></svg>"
#define SVG_RESTORE "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" " \
  "stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">" \
  "<path d=\"M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z\"/>" \
  "<polyline points=\"9 13 12 10 15 13\"/><line x1=\"12\" y1=\"10\" x2=\"12\" y2=\"16\"/></svg>"

// Footer backup/restore controls for the qrg/config forms: a download (backup) link and an upload
// (restore) button, both icon-only with hover tooltips. The upload button opens a hidden file
// picker; selecting a file calls uploadCfgFile() (rdz.js), which confirms, uploads with the forced
// destination name, then reboots. Download is served by GET /file (level-2 auth).
const char *QRG_BACKUP_FOOTER =
  "<input type=\"file\" id=\"qrgupl\" accept=\".txt\" style=\"display:none\" "
    "onchange=\"uploadCfgFile('qrgupl','qrg.txt','frequency list')\">"
  "<span class=\"bkpbtns\">"
    "<a class=\"iconbtn\" href=\"/file/qrg.txt\" download=\"qrg.txt\" title=\"Download backup (qrg.txt)\">" SVG_BACKUP "</a>"
    "<button type=\"button\" class=\"iconbtn\" title=\"Restore from file (qrg.txt)\" "
      "onclick=\"document.getElementById('qrgupl').click()\">" SVG_RESTORE "</button>"
  "</span>";
const char *CONFIG_BACKUP_FOOTER =
  "<input type=\"file\" id=\"cfgupl\" accept=\".txt\" style=\"display:none\" "
    "onchange=\"uploadCfgFile('cfgupl','config.txt','configuration')\">"
  "<span class=\"bkpbtns\">"
    "<a class=\"iconbtn\" href=\"/file/config.txt\" download=\"config.txt\" title=\"Download backup (config.txt)\">" SVG_BACKUP "</a>"
    "<button type=\"button\" class=\"iconbtn\" title=\"Restore from file (config.txt)\" "
      "onclick=\"document.getElementById('cfgupl').click()\">" SVG_RESTORE "</button>"
  "</span>";

const char *handleLoginPost(AsyncWebServerRequest * request) {
  LOG_D(TAG, "Handling login POST request");

  const AsyncWebParameter *userp = request->getParam("user", true, false);
  const AsyncWebParameter *passp = request->getParam("password", true, false);
  if (!userp || !passp) {
    request->send(400, "text/plain", "Invalid Request");
    return nullptr;
  }

  String username = userp->value();
  String password = passp->value();

  int ulvl = verifyPassword(username.c_str(), password.c_str());
  if (ulvl > 0) {
    // Issue a stateless JWT session token (signed with the device key; survives reboot).
    char cookie[COOKIE_SIZE];
    if (jwtCreate(username.c_str(), ulvl, SESSION_TTL_SEC, cookie, sizeof(cookie)) > 0) {
      AsyncWebServerResponse *response = request->beginResponse(302);
      response->addHeader("Location", "/index.html");
      response->addHeader("Set-Cookie", "SESSION=" + String(cookie) + "; Path=/; SameSite=Strict");
      request->send(response);
      return nullptr;
    }
  }
  request->send(401, "text/plain", "Invalid credentials");
  return nullptr;
}

// Extract the SESSION cookie value from a request, or "" if none. dst must hold at least COOKIE_SIZE bytes.
static void getSessionCookie(AsyncWebServerRequest *request, char *dst, int maxlen) {
  dst[0] = 0;
  if(!request->hasHeader("Cookie")) return;
  String cookieHdr = request->getHeader("Cookie")->value();
  // Match "SESSION=" only at a cookie-name boundary (start of header or after a ';'),
  // so we don't accidentally match it inside another cookie name like "MYSESSION=".
  int from = 0;
  while(true) {
    int start = cookieHdr.indexOf("SESSION=", from);
    if(start == -1) return;
    bool boundary = (start == 0);
    if(!boundary) {
      int p = start - 1;
      while(p >= 0 && cookieHdr[p] == ' ') p--;  // skip the separator's whitespace
      boundary = (p >= 0 && cookieHdr[p] == ';');
    }
    if(boundary) {
      start += strlen("SESSION=");
      int end = cookieHdr.indexOf(';', start);
      String session = (end==-1) ? cookieHdr.substring(start) : cookieHdr.substring(start, end);
      session.trim();
      strlcpy(dst, session.c_str(), maxlen);
      return;
    }
    from = start + 1;
  }
}

void handleLogout(AsyncWebServerRequest * request) {
  // Sessions are stateless JWTs (nothing to revoke server-side), so logout just clears
  // the client cookie and returns to the login page. To invalidate ALL sessions at once,
  // rotate the JWT signing key (see the control page).
  AsyncWebServerResponse *response = request->beginResponse(302);
  response->addHeader("Location", "/login.html");
  response->addHeader("Set-Cookie", "SESSION=; Path=/; Max-Age=0; SameSite=Strict");
  request->send(response);
}

// Guard for the user-management endpoints: open only while no users exist yet, so the first
// user can be created on a clean device. Once any named user exists it requires a real
// authenticated level-2 session -- the open default access level is intentionally NOT honored
// here, so user management is locked down as soon as the first account is created.
bool isUserMgmtAllowed(AsyncWebServerRequest * request) {
  if(!hasNamedUsers()) return true;   // clean environment: allow creating the first user
  char session[COOKIE_SIZE];
  getSessionCookie(request, session, COOKIE_SIZE);
  if(session[0] && getCookieAuthLevel(session) >= 2) return true;
  AsyncWebServerResponse *response = request->beginResponse(302);
  response->addHeader("Location", "/login.html");
  request->send(response);
  return false;
}

// Handle add/update/delete of named users (POST /users.html). Access checked by caller.
void handleUsersPost(AsyncWebServerRequest * request) {
  const AsyncWebParameter *actionp = request->getParam("action", true);
  const AsyncWebParameter *userp = request->getParam("user", true);
  if(!actionp || !userp) { request->send(400, "text/plain", "missing parameters"); return; }
  String action = actionp->value();
  String user = userp->value();
  int res = -1;
  if(action == "add") {
    const AsyncWebParameter *levelp = request->getParam("level", true);
    const AsyncWebParameter *passp = request->getParam("pass", true);
    if(!levelp || !passp) { request->send(400, "text/plain", "missing parameters"); return; }
    res = setUser(user.c_str(), levelp->value().toInt(), passp->value().c_str());
  } else if(action == "del") {
    res = deleteUser(user.c_str());
  } else {
    request->send(400, "text/plain", "unknown action");
    return;
  }
  if(res == 0) {
    // Creating the first user closes anonymous access (getDefaultAuthLevel() returns 0
    // once a user is registered). Re-read it so the lockdown takes effect immediately.
    defaultUserLevel = getDefaultAuthLevel();
    request->send(200, "text/plain", "ok");
  }
  else if(res == -2) request->send(409, "text/plain", "cannot remove or demote the last administrator");
  else request->send(400, "text/plain", "error");
}

const char *getQRGAsJson() {
  char *ptr = message;
  strcpy(ptr, "{\"channels\":[");
  for (int i = 0; i < sonde.config.maxsonde; i++) {
    SondeInfo *si = &sonde.sondeList[i];
    if (i > 0) {
      strcat(ptr, ",");
    }
    sprintf(ptr + strlen(ptr),
            "{\"channel\":%d, \"active\":%d, \"freq\":%.3f, \"launchsite\":\"%s\", \"type\":\"%s\"}",
            i+1, si->active, si->freq, si->launchsite, sondeTypeStr[si->type]);
  }
  strcat(ptr, "]}");
  return message;
}

const char *createQRGForm(int level) {
  char *ptr = message;
  HTMLHEAD_V(ptr);
  sprintf(ptr + strlen(ptr), "<script src=\"rdz.js?v=%s\"></script><script src=\"dialog.js?v=%s\"></script></head>", version_id, version_id);
  HTMLBODY(ptr, "qrg.html");
  //strcat(ptr, "<body><form class=\"wrapper\" action=\"qrg.html\" method=\"post\"><div class=\"content\"><table><tr><th>ID</th><th>Active</th><th>Freq</th><th>Launchsite</th><th>Mode</th></tr>");
  strcat(ptr, "<script>\nvar qrgs = [];\n");
  for (int i = 0; i < sonde.config.maxsonde; i++) {
    SondeInfo *si = &sonde.sondeList[i];
    sprintf(ptr + strlen(ptr), "qrgs.push([%d, \"%.3f\", \"%s\", \"%c\"]);\n", si->active, si->freq, si->launchsite, sondeTypeChar[si->type] );
  }
  strcat(ptr, "</script>\n");
  strcat(ptr, "<div id=\"divTable\"></div>");
  strcat(ptr, "<script> qrgTable() </script>\n");
  //</div><div class=\"footer\"><input type=\"submit\" class=\"update\" value=\"Update\"/>");
  // Backup/restore icon buttons live in the footer (see QRG_BACKUP_FOOTER). The hidden file input
  // has no name attribute, so it is never submitted with this page's Save form; picking a file
  // triggers uploadCfgFile (rdz.js), which confirms, uploads as qrg.txt, then reboots.
  HTMLSAVEBUTTON_F(ptr, level, QRG_BACKUP_FOOTER);
  HTMLBODYEND(ptr);
  LOG_D(TAG, "QRG form: size=%d bytes\n", strlen(message));
  return message;
}

const char *handleQRGPost(AsyncWebServerRequest * request) {
  char label[10];
  // parameters: a_i, f_1, t_i  (active/frequency/type)
  File file = LittleFS.open("/qrg.txt", "w");
  if (!file) {
    LOG_E(TAG, "Error while opening '/qrg.txt' for writing");
    return "Error while opening '/qrg.txt' for writing";
  }
  LOG_D(TAG, "Handling post request");
#if 0
  int params = request->params();
  for (int i = 0; i < params; i++) {
    String pname = request->getParam(i)->name();
    Serial.println(pname.c_str());
  }
#endif
  for (int i = 1; i <= sonde.config.maxsonde; i++) {
    snprintf(label, 10, "A%d", i);
    const AsyncWebParameter *active = request->getParam(label, true);
    snprintf(label, 10, "F%d", i);
    const AsyncWebParameter *freq = request->getParam(label, true);
    snprintf(label, 10, "S%d", i);
    const AsyncWebParameter *launchsite = request->getParam(label, true);

    if (!freq) continue;
    snprintf(label, 10, "T%d", i);
    const AsyncWebParameter *type = request->getParam(label, true);
    if (!type) continue;
    String fstring = freq->value();
    String tstring = type->value();
    // launchsite (S%d) is optional in the POST; default to empty if missing
    String sstring = launchsite ? launchsite->value() : String("");
    const char *fstr = fstring.c_str();
    const char *tstr = tstring.c_str();
    const char *sstr = sstring.c_str();
    if (*tstr == '6' || *tstr == '9') tstr = "D";
    LOG_D(TAG, "Processing a=%s, f=%s, t=%s, site=%s\n", active ? "YES" : "NO", fstr, tstr, sstr);
    char typech = tstr[0];
    file.printf("%3.3f %c %c %s\n", atof(fstr), typech, active ? '+' : '-', sstr);
  }
  file.close();

  LOG_D(TAG, "Channel setup finished\n");
  setupChannelList();
  return "";
}


/////////////////// Functions for reading/writing Wifi networks from networks.txt

#define MAX_WIFI 10
int nNetworks;
struct {
  String id;
  String pw;
} networks[MAX_WIFI];


// used by improv wifi
int updateWiFi(String ssid, String pw) {
        networks[1].id = ssid;
        networks[1].pw = pw;
	if(nNetworks<2) nNetworks = 2;
        File file = LittleFS.open("/networks.txt", "w");
        if(!file) return -1;
        for(int i=0; i<nNetworks; i++) {
                if(networks[i].id && networks[i].pw) {
                        file.printf("%s\n%s\n", networks[i].id.c_str(), networks[i].pw.c_str());
                }
        }
        file.close(); 
	return 0;
}       



// FIXME: For now, we don't uspport wifi networks that contain newline or null characters
// ... would require a more sophisicated file format (currently one line SSID; one line Password
void setupWifiList() {
  File file = LittleFS.open("/networks.txt", "r");
  if (!file) {
    LOG_E(TAG, "There was an error opening the file '/networks.txt' for reading");
    networks[0].id = "RDZsonde";
    networks[0].pw = "RDZsonde";
    return;
  }
  int i = 0;

  while (file.available() && i < MAX_WIFI) {
    String line = readLine(file);  //file.readStringUntil('\n');
    if (!file.available()) break;
    networks[i].id = line;
    networks[i].pw = readLine(file); // file.readStringUntil('\n');
    i++;
  }
  nNetworks = i;
  LOG_I(TAG, "%d networks in networks.txt\n", i);
  for (int j = 0; j < i; j++) {
    LOG_I(TAG, "%s: %s\n", networks[j].id.c_str(), networks[j].pw.c_str());
  }
}

// copy string, replacing '"' with '&quot;'
// max string length is 31 characters
const String quoteString(const char *s) {
   char buf[6*32];
   uint16_t i = 0, o = 0;
   int len = strlen(s);
   if(len>31) len=31;
   while(i<len) {
      if(s[i]=='"') { strcpy(buf+o, "&quot;"); o+=6; }
      else buf[o++] = s[i];
      i++;
   }
   buf[o] = 0;
   return String(buf);
}

const char *createWIFIForm() {
  char *ptr = message;
  char tmp[4];
  HTMLHEAD_V(ptr);
  sprintf(ptr + strlen(ptr), "<script src=\"rdz.js?v=%s\"></script></head>", version_id);
  HTMLBODY(ptr, "wifi.html");
  strcat(ptr, "<table><tr><th>Nr</th><th>SSID</th><th>Password</th></tr>");
  for (int i = 0; i < MAX_WIFI; i++) {
    String pw = i < nNetworks ? quoteString( networks[i].pw.c_str() ) : "";
    sprintf(tmp, "%d", i);
    sprintf(ptr + strlen(ptr), "<tr><td>%s</td><td><input name=\"S%d\" type=\"text\" value=\"%s\"/></td>"
            "<td><input name=\"P%d\" type=\"text\" value=\"%s\"/></td>",
            i == 0 ? "<b>AP</b>" : tmp,
            i + 1, i < nNetworks ? networks[i].id.c_str() : "",
            i + 1, pw.c_str() );
  }
  strcat(ptr, "</table><script>footer()</script>");
  //</div><div class=\"footer\"><input type=\"submit\" class=\"update\" value=\"Update\"/>");
  HTMLSAVEBUTTON(ptr, 2);   // WiFi is level-2 only, so the viewer is always an admin
  HTMLBODYEND(ptr);
  LOG_D(TAG, "WIFI form: size=%d bytes\n", strlen(message));
  return message;
}

const char *handleWIFIPost(AsyncWebServerRequest * request) {
  char label[10];
  // parameters: a_i, f_1, t_i  (active/frequency/type)
#if 1
  File f = LittleFS.open("/networks.txt", "w");
  if (!f) {
    LOG_E(TAG, "Error while opening '/networks.txt' for writing");
    return "Error while opening '/networks.txt' for writing";
  }
#endif
  LOG_D(TAG, "Handling post request");
#if 0
  int params = request->params();
  for (int i = 0; i < params; i++) {
    String param = request->getParam(i)->name();
    Serial.println(param.c_str());
  }
#endif
  for (int i = 1; i <= MAX_WIFI; i++) {
    snprintf(label, 10, "S%d", i);
    const AsyncWebParameter *ssid = request->getParam(label, true);
    if (!ssid) continue;
    snprintf(label, 10, "P%d", i);
    const AsyncWebParameter *pw = request->getParam(label, true);
    if (!pw) continue;
    String sstring = ssid->value();
    String pstring = pw->value();
    const char *sstr = sstring.c_str();
    const char *pstr = pstring.c_str();
    if (strlen(sstr) == 0) continue;
    LOG_D(TAG, "Processing S=%s, P=%s\n", sstr, pstr);
    f.printf("%s\n%s\n", sstr, pstr);
  }
  f.close();
  setupWifiList();
  return "";
}

// Show current status
void addSondeStatus(char *ptr, int i)
{
  struct tm ts;
  SondeInfo *s = &sonde.sondeList[i];
  strcat(ptr, "<table class=\"stat\">");
  sprintf(ptr + strlen(ptr), "<tr><td id=\"sfreq\">%3.3f MHz, Type: %s</td><tr><td>ID: %s", s->freq, sondeTypeLongStr[sonde.realType(s)],
          s->d.validID ? s->d.id : "<?""?>");
  if (s->d.validID && (TYPE_IS_DFM(s->type) || TYPE_IS_METEO(s->type) || s->type == STYPE_MP3H) ) {
    sprintf(ptr + strlen(ptr), " (ser: %s)", s->d.ser);
  }
  sprintf(ptr + strlen(ptr), "</td></tr><tr><td>QTH: %.6f,%.6f h=%.0fm</td></tr>\n", s->d.lat, s->d.lon, s->d.alt);
  const time_t t = s->d.time;
  ts = *gmtime(&t);
  sprintf(ptr + strlen(ptr), "<tr><td>Frame# %u, Sats=%d, %04d-%02d-%02d %02d:%02d:%02d</td></tr>",
          s->d.frame, s->d.sats, ts.tm_year + 1900, ts.tm_mon + 1, ts.tm_mday, ts.tm_hour, ts.tm_min, ts.tm_sec);
  if (s->type == STYPE_RS41) {
    sprintf(ptr + strlen(ptr), "<tr><td>Burst-KT=%d Launch-KT=%d Countdown=%d (vor %ds)</td></tr>\n",
            s->d.burstKT, s->d.launchKT, s->d.countKT, ((uint16_t)s->d.frame - s->d.crefKT));
  }
  sprintf(ptr + strlen(ptr), "<tr><td><a target=\"_empty\" href=\"geo:%.6f,%.6f\">GEO-App</a> - ", s->d.lat, s->d.lon);
  sprintf(ptr + strlen(ptr), "<a target=\"_empty\" href=\"https://radiosondy.info/sonde_archive.php?sondenumber=%s\">radiosondy.info</a> - ", s->d.id);
  sprintf(ptr + strlen(ptr), "<a target=\"_empty\" href=\"https://tracker.sondehub.org/%s\">SondeHub Tracker</a> - ", s->d.ser);
  sprintf(ptr + strlen(ptr), "<a target=\"_empty\" href=\"https://www.openstreetmap.org/?mlat=%.6f&mlon=%.6f&zoom=14\">OSM</a> - ", s->d.lat, s->d.lon);
  sprintf(ptr + strlen(ptr), "<a target=\"_empty\" href=\"https://www.google.com/maps/search/?api=1&query=%.6f,%.6f\">Google</a></td></tr>", s->d.lat, s->d.lon);

  strcat(ptr, "</table>\n");
}

const char *createStatusForm() {
  char *ptr = message;
  HTMLHEAD_V(ptr);
  strcat(ptr, "<meta http-equiv=\"refresh\" content=\"5\"></head>");
  HTMLBODY(ptr, "status.html");

  for (int i = 0; i < sonde.config.maxsonde; i++) {
    int snum = (i + sonde.currentSonde) % sonde.config.maxsonde;
    if (sonde.sondeList[snum].active) {
      addSondeStatus(ptr, snum);
    }
  }
  // Close the content div and emit the footer as a sibling (full width, pinned at the
  // bottom) -- same structure as the other forms. Nesting it inside .content would put the
  // footer in the scroll area, so it would scroll and be constrained to the column width.
  strcat(ptr, "</div><div class=\"footer\"><span></span>"
         "<span class=\"ttgoinfo\">rdzTTGOserver ");
  strcat(ptr, version_id);
  strcat(ptr, "</span>");

  HTMLBODYEND(ptr);
  LOG_D(TAG, "Status form: size=%d bytes\n", strlen(message));
  return message;
}

const char *createLiveJson() {
  char *ptr = message;
  SondeInfo *s = &sonde.sondeList[sonde.currentSonde];

  strcpy(ptr, "{\"sonde\": {");
  // use the same JSON format here as for MQTT and for the Android App
  sonde2json( ptr + strlen(ptr), 1024, s );
  // Expose validPos so livemap can tell a fresh fix from a kept/old one (bit 0x80
  // = "position is old"): a frame number can advance without a matching position
  // (RS41 pos subframe CRC fail / all-zeros, DFM missed lat/lon block), and we must
  // not plot that stale position under the newer frame number.
  sprintf(ptr + strlen(ptr), ", \"validPos\": %d", s->d.validPos);
#if 0
  sprintf(ptr + strlen(ptr), "\"sonde\": {\"rssi\": %d, \"vframe\": %d, \"time\": %d,\"id\": \"%s\", \"freq\": %3.3f, \"type\": \"%s\"",
          s->rssi, s->d.vframe, s->d.time, s->d.id, s->freq, sondeTypeStr[sonde.realType(s)]);

  if ( !isnan(s->d.lat) && !isnan(s->d.lon) )
    sprintf(ptr + strlen(ptr), ", \"lat\": %.6f, \"lon\": %.6f", s->d.lat, s->d.lon);
  if ( !isnan(s->d.alt) )
    sprintf(ptr + strlen(ptr), ", \"alt\": %.0f", s->d.alt);
  if ( !isnan(s->d.dir) )
    sprintf(ptr + strlen(ptr), ", \"dir\": %.0f", s->d.dir);
  if ( !isnan(s->d.vs) )
    sprintf(ptr + strlen(ptr), ", \"climb\": %.1f", s->d.vs);
  if ( !isnan(s->d.hs) )
    sprintf(ptr + strlen(ptr), ", \"speed\": %.1f", s->d.hs);

  sprintf(ptr + strlen(ptr), ", \"launchsite\": \"%s\", \"res\": %d }", s->launchsite, s->rxStat[0]);
#endif
  strcat(ptr, " }");

  if (posInfo.valid) {
    sprintf(ptr + strlen(ptr), ", \"gps\": {\"lat\": %.8g, \"lon\": %.8g, \"alt\": %d, \"sat\": %d, \"speed\": %g, \"dir\": %d, \"hdop\": %d }", posInfo.lat, posInfo.lon, posInfo.alt, posInfo.sat, posInfo.speed, posInfo.course, posInfo.hdop);
    //}
  }

  strcat(ptr, "}");
  return message;
}

// Timestamp (millis) of the last /spectrum.json poll. The scan-plot page polls
// every few seconds while open, so a recent value means a browser is watching.
// Written here (web task), read by maybeScanPlotSweep() (RX task); a 32-bit
// aligned load/store is atomic on ESP32, so no lock is needed.
volatile unsigned long lastSpectrumPollMs = 0;

// Auto-scan status snapshot for the web scan-plot. Written by loopAutoScan() in the
// main loop, read by createSpectrumJson() in the web task. Display-only, so the
// occasional mixed read is harmless (same lock-free convention as scandisp[]).
#define AUTOSCAN_MAXPK 16
volatile int autoscanWebState = 0;        // 0=sweeping, 1=trial-decoding
volatile float autoscanWebTryFreq = 0;    // MHz currently being trial-decoded
volatile int autoscanWebTryType = -1;     // SondeType being tried (-1=none)
volatile int autoscanWebNpeaks = 0;       // peaks found in the last sweep
float autoscanWebPeakF[AUTOSCAN_MAXPK];   // detected peak frequencies (MHz)
volatile int autoscanWebPeakN = 0;        // valid entries in autoscanWebPeakF[]

// millis() of the last valid frame from the auto-locked sonde; the locked decode
// holds until norx_timeout seconds elapse with no frame, then re-scans.
unsigned long autoLockGoodMs = 0;

const char *createSpectrumJson() {
  // Reads scandisp[]/peakf lock-free from the web task while the RX task may be
  // sweeping; mirrors createLiveJson(). Display-only data, so a momentarily mixed
  // row is harmless; seq lets the client detect/ignore an in-progress sweep.
  lastSpectrumPollMs = millis();
  char *ptr = message;
  SondeInfo *s = &sonde.sondeList[sonde.currentSonde];
  int n = scanner.dispW();
  const int *data = scanner.dispData();
  int rx = (s->lastState == 1) ? 1 : 0;
  uint32_t lastms = scanner.webMillis();
  unsigned long age = lastms ? (millis() - lastms) : 0;

  ptr += sprintf(ptr,
    "{\"seq\":%u,\"age_ms\":%lu,\"startfreq\":%.5g,\"step\":%.5f,\"n\":%d,"
    "\"noisefloor\":%d,\"peak\":%.3f,\"interval\":%d,\"status\":\"%s\"",
    (unsigned)scanner.webSeq(), age, sonde.config.startfreq, scanner.stepMHz(),
    n, sonde.config.noisefloor, scanner.peakMHz(), sonde.config.scanplotint,
    rx ? "rx" : "idle");

  if (rx) {
    ptr += sprintf(ptr, ",\"rxfreq\":%3.3f,\"rxname\":\"%s\"", s->freq, s->d.id);
  }

  // Auto-scan status (only when running in auto-scan mode)
  if (autoscanActive()) {
    ptr += sprintf(ptr, ",\"autoscan\":1,\"as_state\":\"%s\",\"as_npeaks\":%d",
                   (autoscanWebState == 2) ? "qrg" : (autoscanWebState ? "trial" : "sweep"), autoscanWebNpeaks);
    if (autoscanWebState && autoscanWebTryType >= 0 && autoscanWebTryType < NSondeTypes) {
      ptr += sprintf(ptr, ",\"as_tryfreq\":%.3f,\"as_trytype\":\"%s\"",
                     autoscanWebTryFreq, sondeTypeStr[autoscanWebTryType]);
    }
    ptr += sprintf(ptr, ",\"as_peaks\":[");
    int pn = autoscanWebPeakN; if (pn > AUTOSCAN_MAXPK) pn = AUTOSCAN_MAXPK;
    for (int i = 0; i < pn; i++) ptr += sprintf(ptr, "%s%.3f", i ? "," : "", autoscanWebPeakF[i]);
    ptr += sprintf(ptr, "]");
  }

  // scandisp holds -RssiValue; RSSI[dBm] = -RssiValue/2, so emit data/2.0 as dBm
  // (same convention as the sonde rssi reported to SondeHub). noisefloor is already dBm.
  ptr += sprintf(ptr, ",\"data\":[");
  for (int i = 0; i < n; i++) {
    ptr += sprintf(ptr, "%s%.1f", i ? "," : "", data[i] / 2.0);
  }
  strcpy(ptr, "]}");
  return message;
}
///////////////////// Config form


#if FEATURE_NOTIFY
// Auto-provision a stable, unguessable ntfy topic on first boot when none is configured, so
// the user doesn't have to invent one. Generated once (hardware RNG) and appended to
// /config.txt so it survives reboots -- a per-boot-random topic would break the phone's
// subscription on every restart. It then shows in the config form as the topic to subscribe to.
static void ensureNotifyTopic() {
  if (sonde.config.notify.topic[0] != 0) return;   // already configured or provisioned
  static const char alpha[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  char *t = sonde.config.notify.topic;             // char[48]
  strcpy(t, "rdzsonde-");
  int base = strlen(t);
  for (int i = 0; i < 12; i++) t[base + i] = alpha[esp_random() % 36];
  t[base + 12] = 0;

  // Persist so the topic survives reboots (a per-boot-random topic would break the phone
  // subscription). Rewrite config.txt via a temp file + atomic rename, replacing the
  // existing "notify.topic=" line in place (or appending if none) -- a clean single key,
  // and no risk of concatenating onto a non-newline-terminated last line.
  File in = LittleFS.open("/config.txt", "r");
  File out = LittleFS.open("/config.tmp", "w");
  if (!in || !out) {
    if (in) in.close();
    if (out) { out.close(); LittleFS.remove("/config.tmp"); }
    LOG_W(TAG, "notify: could not persist generated topic '%s' (will regenerate next boot)\n", t);
    return;
  }
  bool replaced = false;
  while (in.available()) {
    String line = readLine(in);                    // trailing \r/\n already stripped
    const char *p = line.c_str();
    while (*p == ' ' || *p == '\t') p++;            // match ignoring leading indentation
    if (strncmp(p, "notify.topic=", 13) == 0) {
      out.printf("notify.topic=%s\n", t);
      replaced = true;
    } else {
      out.print(line); out.print("\n");
    }
  }
  if (!replaced) out.printf("notify.topic=%s\n", t);
  in.close();
  out.close();
  if (LittleFS.rename("/config.tmp", "/config.txt")) {
    LOG_I(TAG, "notify: generated ntfy topic '%s'\n", t);
  } else {
    LittleFS.remove("/config.tmp");
    LOG_W(TAG, "notify: generated topic '%s' but failed to persist (will regenerate next boot)\n", t);
  }
}
#endif

void setupConfigData() {
  File file = LittleFS.open("/config.txt", "r");
  if (!file) {
    LOG_E(TAG, "There was an error opening the file '/config.txt' for reading");
    return;
  }
  while (file.available()) {
    String line = readLine(file);  //file.readStringUntil('\n');
    sonde.setConfig(line.c_str());
  }
  sonde.checkConfig(); // eliminate invalid entries
  file.close();   // release the read handle before ensureNotifyTopic() may rewrite config.txt
#if FEATURE_NOTIFY
  ensureNotifyTopic();
#endif
}


struct st_configitems config_list[] = {
  /* General config settings */
  {"wifi", 0, &sonde.config.wifi},
  {"cachesize", 0, &sonde.config.cachesize},
  {"debug", 0, &sonde.config.debug},
  {"maxsonde", 0, &sonde.config.maxsonde},
  {"rxlat", -7, &sonde.config.rxlat},
  {"rxlon", -7, &sonde.config.rxlon},
  {"rxalt", -7, &sonde.config.rxalt},
  {"b2mute", 0, &sonde.config.b2mute},
  {"screenfile", 0, &sonde.config.screenfile},
  {"display", -6, sonde.config.display},
  {"dispsaver", 0, &sonde.config.dispsaver},
  {"dispcontrast", 0, &sonde.config.dispcontrast},
  /* Spectrum display settings */
  {"spectrum", 0, &sonde.config.spectrum},
  {"startfreq", -7, &sonde.config.startfreq},
  {"channelbw", 0, &sonde.config.channelbw},
  {"marker", 0, &sonde.config.marker},
  {"noisefloor", 0, &sonde.config.noisefloor},
  {"scanplotint", 0, &sonde.config.scanplotint},
  {"scan_smooth", 0, &sonde.config.scan_smooth},
  {"scan_addwait", 0, &sonde.config.scan_addwait},
  {"scan_iter", 0, &sonde.config.scan_iter},
  /* Auto-scan (peak detection) settings; used when autoscan_enable=1 */
  {"autoscan_enable", 0, &sonde.config.autoscan_enable},
  {"autoscan_snr", 0, &sonde.config.autoscan_snr},
  {"autoscan_mindist", 0, &sonde.config.autoscan_mindist},
  {"autoscan_quant", 0, &sonde.config.autoscan_quant},
  {"autoscan_maxpeaks", 0, &sonde.config.autoscan_maxpeaks},
  {"autoscan_dwell", 0, &sonde.config.autoscan_dwell},
  {"autoscan_typedwell", 0, &sonde.config.autoscan_typedwell},
  {"autoscan_qrgfirst", 0, &sonde.config.autoscan_qrgfirst},
  {"autoscan_exclude", 63, &sonde.config.autoscan_exclude},
  {"allowfileupload", 0, &sonde.config.allowfileupload},
  /* decoder settings */
  {"freqofs", 0, &sonde.config.freqofs},
  {"lnaboost", 0, &sonde.config.lnaboost},
  {"lnagain", 0, &sonde.config.lnagain},
  {"rs41.agcbw", 0, &sonde.config.rs41.agcbw},
  {"rs41.rxbw", 0, &sonde.config.rs41.rxbw},
  {"rs92.rxbw", 0, &sonde.config.rs92.rxbw},
  {"rs92.alt2d", 0, &sonde.config.rs92.alt2d},
  {"dfm.agcbw", 0, &sonde.config.dfm.agcbw},
  {"dfm.rxbw", 0, &sonde.config.dfm.rxbw},
  {"m10m20.agcbw", 0, &sonde.config.m10m20.agcbw},
  {"m10m20.rxbw", 0, &sonde.config.m10m20.rxbw},
  {"mp3h.agcbw", 0, &sonde.config.mp3h.agcbw},
  {"mp3h.rxbw", 0, &sonde.config.mp3h.rxbw},
  {"ephftp", 79, &sonde.config.ephftp},
  /* APRS settings */
  {"call", 9, sonde.config.call},
  {"passcode", 0, &sonde.config.passcode},
  /* KISS tnc settings */
  {"kisstnc.active", 0, &sonde.config.kisstnc.active},
#if FEATURE_APRS
  /* AXUDP settings */
  {"axudp.active", -3, &sonde.config.udpfeed.active},
  {"axudp.host", 63, sonde.config.udpfeed.host},
  {"axudp.ratelimit", 0, &sonde.config.udpfeed.ratelimit},
  /* APRS TCP settings */
  {"tcp.active", -3, &sonde.config.tcpfeed.active},
  {"tcp.timeout", 0, &sonde.config.tcpfeed.timeout},
  {"tcp.host", 63, sonde.config.tcpfeed.host},
  {"tcp.host2", 63, &sonde.config.tcpfeed.host2},
  {"tcp.chase", 0, &sonde.config.chase},
  {"tcp.comment", 30, sonde.config.comment},
  {"tcp.objcall", 9, sonde.config.objcall},
  {"tcp.beaconsym", 4, sonde.config.beaconsym},
  {"tcp.highrate", 0, &sonde.config.tcpfeed.highrate},
#endif
#if FEATURE_CHASEMAPPER
  /* Chasemapper settings */
  {"cm.active", -3, &sonde.config.cm.active},
  {"cm.host", 63, &sonde.config.cm.host},
  {"cm.port", 0, &sonde.config.cm.port},
#endif
#if FEATURE_SONDESEEKER
   /* Sondeseeker settings */
   {"ss.active", -3, &sonde.config.ss.active},
   {"ss.host", 63, &sonde.config.ss.host},
   {"ss.port", 0, &sonde.config.ss.port},
#endif
#if FEATURE_NOTIFY
  /* Sonde notifications (ntfy) */
  {"notify.active", 0, &sonde.config.notify.active},
  {"notify.dist", 0, &sonde.config.notify.dist},
  {"notify.alt", 0, &sonde.config.notify.alt},
  {"notify.server", 95, sonde.config.notify.server},
  {"notify.topic", 47, sonde.config.notify.topic},
  {"notify.token", 63, sonde.config.notify.token},
#endif
#if FEATURE_MQTT
  /* MQTT */
  {"mqtt.active", 0, &sonde.config.mqtt.active},
  {"mqtt.id", 63, &sonde.config.mqtt.id},
  {"mqtt.host", 63, &sonde.config.mqtt.host},
  {"mqtt.port", 0, &sonde.config.mqtt.port},
  {"mqtt.username", 63, &sonde.config.mqtt.username},
  {"mqtt.password", 63, &sonde.config.mqtt.password},
  {"mqtt.prefix", 63, &sonde.config.mqtt.prefix},
  {"mqtt.report_interval", 0, &sonde.config.mqtt.report_interval},
#endif
#if FEATURE_SDCARD
  /* SD-Card settings */
  {"sd.cs", 0, &sonde.config.sd.cs},
  {"sd.miso", 0, &sonde.config.sd.miso},
  {"sd.mosi", 0, &sonde.config.sd.mosi},
  {"sd.clk", 0, &sonde.config.sd.clk},
  {"sd.sync", 0, &sonde.config.sd.sync},
  {"sd.name", 0, &sonde.config.sd.name},
  {"sd.speed", 0, &sonde.config.sd.speed},
#endif
#if FEATURE_SERIALOUT
  {"serialout.format", 0, &sonde.config.serialout.format},
  {"serialout.txd", 0, &sonde.config.serialout.txd},
  {"serialout.baud", 0, &sonde.config.serialout.baud},
#endif
  /* Hardware dependeing settings */
  {"disptype", 0, &sonde.config.disptype},
  {"norx_timeout", 0, &sonde.config.norx_timeout},
  {"oled_sda", 0, &sonde.config.oled_sda},
  {"oled_scl", 0, &sonde.config.oled_scl},
  {"oled_rst", 0, &sonde.config.oled_rst},
  {"tft_rs", 0, &sonde.config.tft_rs},
  {"tft_cs", 0, &sonde.config.tft_cs},
  {"tft_orient", 0, &sonde.config.tft_orient},
  {"tft_spifreq", 0, &sonde.config.tft_spifreq},
  {"button_pin", -4, &sonde.config.button_pin},
  {"button2_pin", -4, &sonde.config.button2_pin},
  {"button2_axp", 0, &sonde.config.button2_axp},
  {"touch_thresh", 0, &sonde.config.touch_thresh},
  {"power_pout", 0, &sonde.config.power_pout},
  {"led_pout", 0, &sonde.config.led_pout},
  {"gps_rxd", 0, &sonde.config.gps_rxd},
  {"gps_txd", 0, &sonde.config.gps_txd},
  {"batt_adc", 0, &sonde.config.batt_adc},
#if 1
  {"sx1278_ss", 0, &sonde.config.sx1278_ss},
  {"sx1278_miso", 0, &sonde.config.sx1278_miso},
  {"sx1278_mosi", 0, &sonde.config.sx1278_mosi},
  {"sx1278_sck", 0, &sonde.config.sx1278_sck},
#endif
  {"mdnsname", 14, &sonde.config.mdnsname},

#if FEATURE_SONDEHUB
  /* SondeHub settings */
  {"sondehub.active", 0, &sonde.config.sondehub.active},
  {"sondehub.chase", 0, &sonde.config.sondehub.chase},
  {"sondehub.host", 63, &sonde.config.sondehub.host},
  {"sondehub.callsign", 63, &sonde.config.sondehub.callsign},
  {"sondehub.antenna", 63, &sonde.config.sondehub.antenna},
  {"sondehub.email", 63, &sonde.config.sondehub.email},
  {"sondehub.fiactive", 0, &sonde.config.sondehub.fiactive},
  {"sondehub.fiinterval", 0, &sonde.config.sondehub.fiinterval},
  {"sondehub.fimaxdist", 0, &sonde.config.sondehub.fimaxdist},
  {"sondehub.fimaxage", -7, &sonde.config.sondehub.fimaxage},
#endif
};

const int N_CONFIG = (sizeof(config_list) / sizeof(struct st_configitems));

const char *createConfigForm(int level) {
  char *ptr = message;
  HTMLHEAD_V(ptr);
  sprintf(ptr + strlen(ptr), "<script src=\"rdz.js?v=%s\"></script><script src=\"dialog.js?v=%s\"></script></head>", version_id, version_id);
  HTMLBODY_OS(ptr, "config.html", "return checkForDuplicates(this)");
  strcat(ptr, "<div id=\"cfgtab\"></div>");
  sprintf(ptr + strlen(ptr), "<script src=\"cfg.js?v=%s\"></script>", version_id);
  strcat(ptr, "<script>\n");
  sprintf(ptr + strlen(ptr), "var scr=\"Using /screens%d.txt", Display::getScreenIndex(sonde.config.screenfile));
  for (int i = 0; i < disp.nLayouts; i++) {
    sprintf(ptr + strlen(ptr), "<br>%d=%s", i, disp.layouts[i].label);
  }
  strcat(ptr, "\";\n");
  strcat(ptr, "var cf=new Map();\n");
  for (int i = 0; i < N_CONFIG; i++) {
    sprintf(ptr + strlen(ptr), "cf.set(\"%s\", \"", config_list[i].name);
    switch (config_list[i].type) {
      case -4:
      case -3:
      case -2:
      case 0:
        sprintf(ptr + strlen(ptr), "%d", *(int *)config_list[i].data);
        LOG_D(TAG, "Config for %s is %d\n", config_list[i].name, *(int *)config_list[i].data);
        break;
      case -6: // list
        {
          int8_t *l = (int8_t *)config_list[i].data;
          if (*l == -1) strcat(ptr, "0");
          else {
            sprintf(ptr + strlen(ptr), "%d", l[0]);
            l++;
          }
          while (*l != -1) {
            sprintf(ptr + strlen(ptr), ",%d", *l);
            l++;
          }
        }
        break;
      case -7: // double
        if (!isnan(*(double *)config_list[i].data))
          sprintf(ptr + strlen(ptr), "%.8g", *(double *)config_list[i].data);
        break;
      default: // string
        strcat(ptr, (char *)config_list[i].data);
    }
    strcat(ptr, "\");\n");
  }
  strcat(ptr, "configTable();\n </script>");
  strcat(ptr, "<script>footer()</script>");
  // Backup/restore icon buttons live in the footer (see CONFIG_BACKUP_FOOTER). The hidden file
  // input has no name attribute, so it is never submitted with this page's Save form; picking a
  // file triggers uploadCfgFile (rdz.js), which confirms, uploads as config.txt, then reboots.
  HTMLSAVEBUTTON_F(ptr, level, CONFIG_BACKUP_FOOTER);
  HTMLBODYEND(ptr);
  LOG_D(TAG, "Config form: size=%d bytes\n", strlen(message));
  return message;
}


const char *handleConfigPost(AsyncWebServerRequest * request) {
  // parameters: a_i, f_1, t_i  (active/frequency/type)
  LOG_D(TAG, "Handling config post request");
#if 1
  File f = LittleFS.open("/config.txt", "w");
  if (!f) {
    LOG_E(TAG, "Error while opening '/config.txt' for writing");
    return "Error while opening '/config.txt' for writing";
  }
#endif
  LOG_D(TAG, "File open for writing.");
  int params = request->params();
#if 0
  for (int i = 0; i < params; i++) {
    String param = request->getParam(i)->name();
    Serial.println(param.c_str());
  }
#endif
  for (int i = 0; i < params; i++) {
    String strlabel = request->getParam(i)->name();
    const char *label = strlabel.c_str();
    size_t labellen = strlen(label);
    if (labellen == 0) continue;	// empty parameter name => label[-1] read
    if (label[labellen - 1] == '#') continue;
    const AsyncWebParameter *value = request->getParam(label, true);
    if (!value) continue;
    String strvalue = value->value();
    if ( strcmp(label, "button_pin") == 0 ||
         strcmp(label, "button2_pin") == 0) {
      const AsyncWebParameter *touch = request->getParam(strlabel + "#", true);
      if (touch) {
        int i = atoi(strvalue.c_str());
        if (i != -1 && i != 255) i += 128;
        strvalue = String(i);
      }
    }
    LOG_D(TAG, "Processing %s=%s\n", label, strvalue.c_str());
    //int wlen = f.printf("%s=%s\n", config_list[idx].name, strvalue.c_str());
    int wlen = f.printf("%s=%s\n", label, strvalue.c_str());
    LOG_D(TAG, "Written bytes: %d\n", wlen);
  }
  // allowfileupload is a hidden option (not rendered in cfg.js), so it is never part of
  // the submitted form. Re-emit it when enabled so saving the config form does not wipe it.
  if (sonde.config.allowfileupload) {
    f.printf("allowfileupload=%d\n", sonde.config.allowfileupload);
  }
  LOG_D(TAG, "Flushing file\n");
  f.flush();
  LOG_D(TAG, "Closing file\n");
  f.close();
  LOG_D(TAG, "Re-reading file file\n");
  setupConfigData();
  if (!gpsPos.valid) fixedToPosInfo();
  // TODO: Check if this is better done elsewhere?
  // Use new config (whereever this is feasible without a reboot)
  disp.setContrast();
  return "";
}

const char *ctrlid[] = {"rx", "scan", "spec", "wifi", "rx2", "scan2", "spec2", "wifi2",
#if FEATURE_RS92
	"rinex",
#endif
#if FEATURE_SDCARD
	"format",
#endif
        "reboot"};

const char *ctrllabel[] = {"Receiver/next freq. (short keypress)", "Scanner (double keypress)", "Spectrum (medium keypress)", "WiFi (long keypress)",
                           "Button 2/next screen (short keypress)", "Button 2 (double keypress)", "Button 2 (medium keypress)", "Button 2 (long keypress)",
#if FEATURE_RS92
                           "Update RS92 RINEX eph",
#endif
#if FEATURE_SDCARD
			   "Format SD Card",
#endif
			   "Reboot"
                          };

// Human-readable description of a display action code (see ACT_* in Sonde.h).
// Used to label the control-page keypress buttons with what each press actually
// does on the screen the device is currently showing.
static const char *actionDescr(uint8_t act) {
  switch (act) {
    case ACT_NONE:             return "no function";
    case ACT_DISPLAY_SCANNER:  return "scanner";
    case ACT_DISPLAY_WIFI:     return "WiFi screen";
    case ACT_DISPLAY_SPECTRUM: return "spectrum";
    case ACT_DISPLAY_DEFAULT:  return "default screen";
    case ACT_DISPLAY_NEXT:     return "next screen";
    case ACT_NEXTSONDE:        return "next frequency";
    case ACT_PREVSONDE:        return "previous frequency";
    case ACT_RINEX_UPDATE:     return "update RINEX";
    case ACT_FORMAT_SD:        return "format SD card";
  }
  static char abuf[20];
  if (act < ACT_MAXDISPLAY) snprintf(abuf, sizeof(abuf), "screen %d", act);
  else                      snprintf(abuf, sizeof(abuf), "action %d", act);
  return abuf;
}

// True if action 'act' switches the active display to a different screen. Such a press
// changes what every control button does, so the control page must reload to relabel them.
static bool actionChangesScreen(uint8_t act) {
  if (act == ACT_DISPLAY_NEXT || act == ACT_DISPLAY_DEFAULT) return true;
  return act < ACT_MAXDISPLAY;   // ACT_DISPLAY(n): jump to a specific screen (incl. scanner)
}

const char *createControlForm(int authLevel, bool reloadAfter) {
  char *ptr = message;
  HTMLHEAD_V(ptr);
  // confirmSubmit() guards the destructive/disruptive control buttons (Format SD, Reboot) with
  // the project's styled confirmation. It cancels the immediate submit, shows showConfirm(), and
  // -- only on accept -- re-submits the form with a hidden field carrying the button's name (a
  // programmatic submit() would otherwise drop the clicked submit button's name/value).
  strcat(ptr, "<script src=\"dialog.js?v=");
  strcat(ptr, version_id);
  strcat(ptr, "\"></script><script>function confirmSubmit(b,msg){"
              "showConfirm(msg).then(function(ok){if(!ok)return;"
              "var h=document.createElement('input');h.type='hidden';h.name=b.name;h.value=b.value;"
              "b.form.appendChild(h);b.form.submit();});return false;}"
              // Reboot uses the same reload logic as firmware update / config restore: confirm,
              // capture the boot nonce, fire the reboot POST (no response -- the device restarts at
              // once), then poll /bootid and reload the page once it is back online.
              "function confirmReboot(msg){showConfirm(msg).then(function(ok){if(!ok)return;"
              "fetch('/bootid',{cache:'no-store'}).then(function(r){return r.ok?r.text():'';})"
              ".catch(function(){return '';}).then(function(before){"
              "fetch('/control.html',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'reboot=1'}).catch(function(){});"
              "waitForRebootAndReload((before||'').trim(),'Rebooting','The device is rebooting.');});});return false;}</script>");
  strcat(ptr, "</head>");
  HTMLBODY(ptr, "control.html");
  if (reloadAfter) {
    // The press just queued a screen switch; the main loop applies it asynchronously.
    // Reload (GET, so the press is not repeated) shortly after, to show the new labels.
    strcat(ptr, "<script>setTimeout(function(){location.href='/control.html';},700);</script>");
  }
  // The first 8 control buttons (ctrlid rx/scan/spec/wifi + rx2/scan2/spec2/wifi2) map to the
  // key1/key2 short/double/medium/long actions[1..8] of the current screen. Label them with
  // what they actually do right now (function first, then which button/keypress triggers it),
  // and disable the ones with no function; entries >= 8 keep their static label (RINEX/Format/Reboot).
  static const char *kpName[] = {"short", "double", "medium", "long"};
  char dynlabel[64];
  for (int i = 0; i < sizeof(ctrllabel)/sizeof((ctrllabel)[0]); i++) {
#if FEATURE_SDCARD
    // Formatting the SD card is destructive: only offer it to admin (level 2) users.
    if (strcmp(ctrlid[i], "format") == 0 && authLevel < 2) continue;
#endif
    const char *label = ctrllabel[i];
    bool disabled = false;
    if (i < 8) {
      uint8_t act = disp.layout ? disp.layout->actions[i + 1] : ACT_NONE;
      snprintf(dynlabel, sizeof(dynlabel), "%s (button %d %s keypress)",
               actionDescr(act), (i < 4) ? 1 : 2, kpName[i & 3]);
      // Capitalize the first letter so it reads as a button caption.
      if (dynlabel[0] >= 'a' && dynlabel[0] <= 'z') dynlabel[0] -= ('a' - 'A');
      label = dynlabel;
      disabled = (act == ACT_NONE);   // nothing happens on this press -> grey it out
    }
    strcat(ptr, "<input class=\"ctlbtn\" type=\"submit\" name=\"");
    strcat(ptr, ctrlid[i]);
    strcat(ptr, "\" value=\"");
    strcat(ptr, label);
    strcat(ptr, "\"");                        // close the value attribute
    // Destructive/disruptive actions get a styled confirmation before the form submits.
#if FEATURE_SDCARD
    if (strcmp(ctrlid[i], "format") == 0)
      strcat(ptr, " onclick=\"return confirmSubmit(this,'Format the SD card?\\nThis permanently erases all data on the card.');\"");
#endif
    if (strcmp(ctrlid[i], "reboot") == 0)
      strcat(ptr, " onclick=\"return confirmReboot('Reboot the device now?');\"");
    strcat(ptr, disabled ? " disabled></input>" : "></input>");
    if (i == 3 || i == 7 ) {
      strcat(ptr, "<p></p>");
    }
  }
  strcat(ptr, "</div><div class=\"footer\"><span></span>"
         "<span class=\"ttgoinfo\">rdzTTGOserver ");
  strcat(ptr, version_id);
  strcat(ptr, "</span>");
  HTMLBODYEND(ptr);
  LOG_I(TAG, "Control form: size=%d bytes\n", strlen(message));
  return message;
}


// Handle a control-page POST. Returns true if the press switched the active screen, so the
// caller knows the control page should reload to relabel the (now changed) buttons.
bool handleControlPost(AsyncWebServerRequest * request, int authLevel) {
  LOG_D(TAG, "Handling control post request");
  bool screenChanged = false;
  int params = request->params();
  for (int i = 0; i < params; i++) {
    String param = request->getParam(i)->name();
    LOG_D(TAG, "Control post: %s\n", param.c_str());
    // The 8 keypress buttons ctrlid[0..7] (rx/scan/spec/wifi + rx2/scan2/spec2/wifi2) map to
    // button1/button2 short/double/medium/long, i.e. actions[1..8] of the current screen.
    int kp = -1;
    for (int k = 0; k < 8; k++) {
      if (param.equals(ctrlid[k])) { kp = k; break; }
    }
    if (kp >= 0) {
      Button *b = (kp < 4) ? &button1 : &button2;
      b->pressed = (KeyPress)(KP_SHORT + (kp & 3));   // KP_SHORT/DOUBLE/MID/LONG
      uint8_t act = disp.layout ? disp.layout->actions[kp + 1] : ACT_NONE;
      if (actionChangesScreen(act)) screenChanged = true;
    }
    else if (param.equals("rinex")) {
      button2.pressed = KP_RINEX;
    }
    else if (param.equals("format")) {
      // Formatting the SD card is destructive: only admin (level 2) users may trigger it.
      if (authLevel >= 2) button2.pressed = KP_FORMAT;
      else LOG_W(TAG, "Rejected SD format request: insufficient auth level (%d)\n", authLevel);
    }
    else if (param.equals("reboot")) {
      ESP.restart();
    }
    else if (param.equals("logout_all")) {
      // Rotate the JWT signing key so every existing session token becomes invalid.
      if (authLevel >= 2) rotateJwtKey();
      else LOG_W(TAG, "Rejected logout-all request: insufficient auth level (%d)\n", authLevel);
    }
  }
  return screenChanged;
}

void handleUpload(AsyncWebServerRequest * request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
  static File file;
  if (!index) {
    const char *fn = filename.c_str();
    if(!checkAllowed(fn)) {
      LOG_E(TAG, "UploadStart: writing %s prohibited\n", fn);
      return;
    }
    LOG_D(TAG, "UploadStart: %s\n", filename.c_str());
    file = LittleFS.open("/" + filename, "w");
    if (!file) {
      LOG_E(TAG, "Error opening the file '/%s' for writing", fn);
    }
  }
  if (!file) return;
  for (size_t i = 0; i < len; i++) {
    file.write(data[i]);
  }
  if (final) {
    LOG_D(TAG, "UploadEnd: %s, %u B\n", filename.c_str(), index + len);
    file.close();
  }
}


int streamEditForm(int &state, File & file, String filename, char *buffer, size_t maxlen, size_t index) {
  LOG_D(TAG, "streamEdit: state=%d  max:%d idx:%d\n", state, maxlen, index);
  int i = 0;
  switch (state) {
    case 0: // header
      {
        // we optimistically assume that on first invocation, maxlen is large enough to handle the header.....
        strncpy(buffer, "<html><head><title>Editor</title></head><body><p>Edit: ", maxlen);
        i = strlen(buffer);
        strncpy(buffer + i, filename.c_str(), maxlen - i);
        i += strlen(buffer + i);
        strncpy(buffer + i, "</p><form action=\"edit.html?file=", maxlen - i);
        i += strlen(buffer + i);
        strncpy(buffer + i, filename.c_str(), maxlen - i);
        i += strlen(buffer + i);
        strncpy(buffer + i, "\" method=\"post\" enctype=\"multipart/form-data\"><textarea name=\"text\" cols=\"80\" rows=\"40\">", maxlen - i);
        i += strlen(buffer + i);
        if (i >= maxlen) {
          strncpy(buffer, "Out of memory", maxlen);
          state = 3;
          return strlen(buffer);
        }
        state++;
        LOG_D(TAG, "Wrote %d bytes. Header finished", i);
        return i;
        break;
      }
    case 1: // file content
      while (file.available()) {
        int cnt = readLine(file, buffer + i, maxlen - i - 1);
        i += cnt;
        if (i + 2 > maxlen) break; // no room for '\n'+NUL; readLine already terminated buffer
        buffer[i++] = '\n';
        buffer[i] = 0;
        if (i + 256 > maxlen) break; // max line length in file 256 chars
      }
      if (i > 0) return i;
      file.close();
      state++;  // intentional fall-through
    case 2:  // footer
      Serial.println("Appending footer\n");
      strncpy(buffer, "</textarea><input type=\"submit\" value=\"Save\"></input></form></body></html>", maxlen);
      state++;
      return strlen(buffer);
    case 3:  // end
      return 0;
  }
  return 0;
}

// bad idea. prone to buffer overflow. use at your own risk...
const char *createEditForm(String filename) {
  Serial.println("Creating edit form");
  char *ptr = message;
  File file = LittleFS.open("/" + filename, "r");
  if (!file) {
    Serial.println("There was an error opening the file '/config.txt' for reading");
    return "<html><head><title>File not found</title></head><body>File not found</body></html>";
  }

  strcpy(ptr, "<html><head><title>Editor ");
  strcat(ptr, filename.c_str());
  strcat(ptr, "</title></head><body><form action=\"edit.html?file=");
  strcat(ptr, filename.c_str());
  strcat(ptr, "\" method=\"post\" enctype=\"multipart/form-data\">");
  strcat(ptr, "<textarea name=\"text\" cols=\"80\" rows=\"40\">");
  while (file.available()) {
    String line = readLine(file);  //file.readStringUntil('\n');
    strcat(ptr, line.c_str()); strcat(ptr, "\n");
  }
  strcat(ptr, "</textarea><input type=\"submit\" value=\"Save\"></input></form></body></html>");
  LOG_D(TAG, "Edit form: size=%d bytes\n", strlen(message));
  return message;
}


const char *handleEditPost(AsyncWebServerRequest * request) {
  Serial.println("Handling post request");
  int params = request->params();
  LOG_D(TAG, "Post:, %d params\n", params);
  for (int i = 0; i < params; i++) {
    const AsyncWebParameter* p = request->getParam(i);
    String name = p->name();
    String value = p->value();
    if (name.c_str() == NULL) {
      name = String("NULL");
    }
    if (value.c_str() == NULL) {
      value = String("NULL");
    }
    if (p->isFile()) {
      LOG_D(TAG, "_FILE[%s]: %s, size: %u\n", name.c_str(), value.c_str(), p->size());
    } else if (p->isPost()) {
      LOG_D(TAG, "_POST[%s]: %s\n", name.c_str(), value.c_str());
    } else {
      LOG_D(TAG, "_GET[%s]: %s\n", name.c_str(), value.c_str());
    }
  }

  const AsyncWebParameter *filep = request->getParam("file");
  if (!filep) return NULL;

  String filename = filep->value();
  const char *fn = filename.c_str();
  if(!checkAllowed(fn)) {
    LOG_E(TAG, "handleEditPost: writing %s prohibited\n", fn);
    return NULL;
  }

  LOG_D(TAG, "Writing file <%s>\n", fn);
  const AsyncWebParameter *textp = request->getParam("text", true);
  if (!textp) return NULL;
  LOG_D(TAG, "Parameter size is %d\n", textp->size());
  LOG_D(TAG, "Multipart: %d  contentlen=%d  \n",
                request->multipart(), request->contentLength());
  String content = textp->value();
  if (content.length() == 0) {
    Serial.println("File is empty. Not written.");
    return NULL;
  }
  File file = LittleFS.open("/" + filename, "w");
  if (!file) {
    Serial.println("There was an error opening the file '/" + filename + "'for writing");
    return "";
  }
  LOG_D(TAG, "File is open for writing, content is %d bytes\n", content.length());
  int len = file.print(content);
  file.close();
  LOG_D(TAG, "Written: %d bytes\n", len);
  if (strncmp(filename.c_str(), "screens", 7) == 0) {
    // screens update => reload
    forceReloadScreenConfig = true;
  }
  return "";
}

// will be removed. its now in data/upd.html (for GET; POST to update.html still handled here)
const char *createUpdateForm(boolean run) {
  char *ptr = message;
  sprintf(ptr, "<!DOCTYPE html><html><head>"
               "<meta name=\"color-scheme\" content=\"light dark\">"
               "<script src=\"theme.js?v=%s\"></script>"
               "<link rel=\"stylesheet\" type=\"text/css\" href=\"style.css?v=%s\"></head>"
               "<body><form action=\"update.html\" method=\"post\">", version_id, version_id);
  if (run) {
    strcat(ptr, "<p>Doing update, wait until reboot</p>");
  } else {
    sprintf(ptr + strlen(ptr), "<p>Currently installed: %s-%c%d</p>\n", version_id, FS_MAJOR + 'A' - 1, FS_MINOR);
    strcat(ptr, "<p>Available main: <iframe src=\"http://rdzsonde.org/main/update-info.html\" style=\"height:40px;width:400px\"></iframe><br>"
           "Available devel: <iframe src=\"http://rdzsonde.org/dev2/update-info.html\" style=\"height:40px;width:400px\"></iframe></p>");
    strcat(ptr, "<input type=\"submit\" name=\"main\" value=\"Main-Update\"></input><br><input type=\"submit\" name=\"dev\" value=\"Devel-Update\">");
    strcat(ptr, "<br><p>Note: If suffix is the same, update should work fully. If the number is different, update contains changes in the file system. A full re-flash is required to get all new features, but the update should not break anything. If the letter is different, a full re-flash is mandatory, update will not work</p>");
  }
  strcat(ptr, "</form></body></html>");
  LOG_D(TAG, "Update form: size=%d bytes\n", strlen(message));
  return message;
}

const char *handleUpdatePost(AsyncWebServerRequest * request) {
  Serial.println("Handling post request");
  int params = request->params();
  bool updateFromURL = false;
  for (int i = 0; i < params; i++) {
    String param = request->getParam(i)->name();
    Serial.println(param.c_str());
    if (param.equals("dev2")) {
      Serial.println("equals devel");
      updateHost = updateHostOfficial;
      updatePort = 80;
      updatePrefix = updatePrefixD;
    }
    else if (param.equals("main")) {
      Serial.println("equals main");
      updateHost = updateHostOfficial;
      updatePort = 80;
      updatePrefix = updatePrefixM;
    }
    else if (param.equals("py5ol")) {
      Serial.println("equals py5ol");
      updateHost = updateHostPy5ol;
      updatePort = 80;
      updatePrefix = updatePrefixP;
    }
    else if (localUpdates && param.equals("local")) {
      // Local updates permitted. Expect URL as url parameter...
      updateFromURL = true;
    }
    else if (updateFromURL && param.equals("url")) {
      // Note: strdup allocates memory that is never free'd.
      // Let's not care about this as we are going to reboot after the update anyway...
      String localsrc = request->getParam(i)->value();
      int pos;

      if (-1 != (pos = localsrc.indexOf("://")) ){
        // strip off http://
        localsrc = localsrc.substring(pos+3);
      }

      if (-1 != (pos = localsrc.indexOf("/")) ){
        // see if there's a directory or updates are in the root
        String tmp = localsrc.substring(pos);
        updatePrefix = strdup(tmp.c_str());
        localsrc = localsrc.substring(0, pos);
      } else {
         updatePrefix = strdup("/");
      }

      if (-1 != (pos = localsrc.indexOf(":"))) {
        // extract port
        updatePort = atoi(localsrc.substring(pos+1).c_str());
        updateHost = strdup(localsrc.substring(0, pos).c_str());
      } else {
        updateHost = strdup(localsrc.c_str());
      }
    }
  }
  LOG_I(TAG, "Updating: %s%s from %s:%d\n", updatePrefix, updateIno, updateHost, updatePort);
  enterMode(ST_UPDATE);
  return "";
}

const char *createKMLLive(const char *myIP) {
  char *ptr = message;

  strcpy(ptr, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><kml xmlns=\"http://www.opengis.net/kml/2.2\"><NetworkLink><name>loads dynamic.kml</name><Link><href>http://");
  strcat(ptr, myIP);
  strcat(ptr, "/dynamic.kml</href><refreshMode>onInterval</refreshMode><refreshInterval>10</refreshInterval></Link></NetworkLink></kml>");

  return message;
}

void addSondeStatusKML(char *ptr, int i)
{
  SondeInfo *s = &sonde.sondeList[i];

  if (!s->d.validID)
  {
    return;
  }

  sprintf(ptr + strlen(ptr), "<Placemark id=\"%s\"><name>%s</name><Point><altitudeMode>absolute</altitudeMode><coordinates>%.6f,%.6f,%.0f</coordinates></Point><description>%3.3f MHz, Type: %s, h=%.0fm</description></Placemark>",
          s->d.id, s->d.id,
          s->d.lon, s->d.lat, s->d.alt,
          s->freq, sondeTypeStr[sonde.realType(s)], s->d.alt);
}

const char *createKMLDynamic() {
  char *ptr = message;

  strcpy(ptr, "<?xml version=\"1.0\" encoding=\"UTF-8\"?><kml xmlns=\"http://www.opengis.net/kml/2.2\"><Document>");

  for (int i = 0; i < sonde.config.maxsonde; i++) {
    int snum = (i + sonde.currentSonde) % sonde.config.maxsonde;
    if (sonde.sondeList[snum].active) {
      addSondeStatusKML(ptr, snum);
    }
  }

  strcat(ptr, "</Document></kml>");

  return message;
}


const char *sendGPX(AsyncWebServerRequest * request) {
  Serial.println("\n\n\n********GPX request\n\n");
  String url = request->url();
  int index = atoi(url.c_str() + 1);
  char *ptr = message;
  if (index < 0 || index >= MAXSONDE) {
    return "ERROR";
  }
  SondeInfo *si = &sonde.sondeList[index];
  snprintf(ptr, 10240, "<?xml version='1.0' encoding='UTF-8'?>\n"
           "<gpx version=\"1.1\" creator=\"http://rdzsonde.local\" xmlns=\"http://www.topografix.com/GPX/1/1\" "
           "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
           "xsi:schemaLocation=\"http://www.topografix.com/GPX/1/1 http://www.topografix.com/GPX/1/1/gpx.xsd\">\n"
           "<metadata>"
           "<name>Sonde #%d (%s)</name>\n"
           "<author>rdzTTGOsonde</author>\n"
           "</metadata>\n"
           "<wpt lat=\"%f\" lon=\"%f\">\n  <ele>%f</ele>\n  <name>%s</name>\n  <sym>Radio Beacon</sym><type>Sonde</type>\n"
           "</wpt></gpx>\n", index, si->d.id, si->d.lat, si->d.lon, si->d.alt, si->d.id);
  Serial.println(message);
  return message;
}

bool isAuthenticated(AsyncWebServerRequest *request, int level) {
  if(defaultUserLevel >= level)
    return 1;
  char session[COOKIE_SIZE];
  getSessionCookie(request, session, COOKIE_SIZE);
  if(session[0]) {
    int ulvl = getCookieAuthLevel(session);
    if(ulvl >= level) {
      return 1;
    }
  }
  // Not authenticated: send the user to the login page instead of a bare 401 text response.
  // Remember where they were headed as ?next= so login.html can return them there afterwards.
  // request->url() is the URL-decoded path only (query already stripped). Only carry simple,
  // safe local paths; login.html validates again, and anything unusual falls back to /index.html.
  AsyncWebServerResponse *response = request->beginResponse(302);
  String loc = "/login.html";
  String url = request->url();
  bool safe = url.length() > 1 && url[0] == '/' && url != "/login.html";
  for(unsigned int i = 0; safe && i < url.length(); i++) {
    char c = url[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '/' || c == '.' || c == '-' || c == '_';
    if(!ok) safe = false;
  }
  if(safe) loc += "?next=" + url;
  response->addHeader("Location", loc);
  request->send(response);
  return false;
}

// Effective access level for a request: the larger of the open default level and the
// session level (same rule isAuthenticated/whoami use). Used to render UI per access level.
int reqAuthLevel(AsyncWebServerRequest *request) {
  int level = defaultUserLevel;
  char session[COOKIE_SIZE];
  getSessionCookie(request, session, COOKIE_SIZE);
  if(session[0]) {
    int slvl = getCookieAuthLevel(session);
    if(slvl > level) level = slvl;
  }
  return level;
}

const char* PARAM_MESSAGE = "message";

#if FEATURE_SDCARD
static bool deleteSdDirRecursive(const char *sdPath, const char *vfsPath) {
  char entryPath[360];
  char vfsFull[360];
  DIR *d = opendir(vfsPath);
  if(!d) return false;
  struct dirent *e;
  struct stat st;
  while((e = readdir(d)) != NULL) {
    if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    snprintf(entryPath, sizeof(entryPath), "%s/%s", sdPath, e->d_name);
    snprintf(vfsFull, sizeof(vfsFull), "%s/%s", vfsPath, e->d_name);
    if(stat(vfsFull, &st) == 0 && (st.st_mode & S_IFDIR)) {
      if(!deleteSdDirRecursive(entryPath, vfsFull)) { closedir(d); return false; }
    } else {
      SD.remove(entryPath);
    }
  }
  closedir(d);
  return SD.rmdir(sdPath);
}
#endif

// Unpack a filesystem-update archive (the format produced by scripts/makefsupdate.py)
// from a stream into LittleFS. The archive repeats a "<filename> <size>\n" header line
// followed by <size> raw bytes; each entry is written to /<filename>. Works on any
// Stream -- the pull OTA passes the WiFiClient, the web upload passes a buffered File.
// Returns the number of files written, or -1 on a malformed header.
int unpackFsArchive(Stream &in) {
  int count = 0;
  while (in.available()) {
    char fn[128];
    fn[0] = '/';
    size_t fnlen = in.readBytesUntil('\n', fn + 1, sizeof(fn) - 2);
    fn[1 + fnlen] = 0;   // readBytesUntil does not terminate; also keeps the write in bounds
    char *sz = strchr(fn, ' ');
    if (!sz) return -1;
    *sz = 0;
    int len = atoi(sz + 1);
    LOG_I(TAG, "Updating file %s (%d bytes)\n", fn, len);
    File f = LittleFS.open(fn, FILE_WRITE);
    while (len > 0) {
      unsigned char buf[1024];
      size_t r = in.readBytes((char *)buf, len > 1024 ? 1024 : len);
      if (r == 0) break;   // timeout / end of stream -- stop this entry
      if (f) f.write(buf, r);
      len -= r;
    }
    if (f) f.close();
    count++;
  }
  return count;
}

// --- Web file-upload OTA (POST /uploadota) -------------------------------------------
// Deferred reboot: a millis() deadline set by the upload completion handler; loop()
// restarts once it passes, so the HTTP response is delivered before the reboot.
unsigned long otaRebootAt = 0;
// State for the single in-flight /uploadota request. A request may carry the firmware
// bin and/or the filesystem archive; each file part is routed by its leading byte.
static bool otaUpInProgress = false, otaUpErr = false, otaUpFw = false, otaUpFs = false;
static int  otaUpTarget = 0;          // current part: 1=firmware (U_FLASH), 2=filesystem temp file
static File otaUpFsFile;
#define OTA_FS_TMP "/_otafs.bin"

// Per-chunk upload callback. ESP32 app images begin with 0xE9; anything else is treated
// as a filesystem archive (buffered to a temp file, unpacked in the completion handler).
void handleOtaUpload(AsyncWebServerRequest *request, const String &filename, size_t index,
                     uint8_t *data, size_t len, bool final) {
  if (index == 0 && !otaUpInProgress) {   // first part of the request -> reset state
    otaUpInProgress = true; otaUpErr = false; otaUpFw = false; otaUpFs = false;
  }
  if (index == 0) {
    // Silent auth + feature gate (must not write firmware for unauthorized requests).
    if (reqAuthLevel(request) < 2 || !sonde.config.allowfileupload) { otaUpErr = true; otaUpTarget = 0; return; }
    if (len > 0 && data[0] == 0xE9) {
      otaUpTarget = 1; otaUpFw = true;
      LOG_I(TAG, "OTA upload: firmware '%s'\n", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { Update.printError(Serial); otaUpErr = true; otaUpTarget = 0; }
    } else {
      otaUpTarget = 2; otaUpFs = true;
      LOG_I(TAG, "OTA upload: filesystem '%s'\n", filename.c_str());
      otaUpFsFile = LittleFS.open(OTA_FS_TMP, "w");
      if (!otaUpFsFile) { otaUpErr = true; otaUpTarget = 0; }
    }
  }
  if (otaUpErr) return;
  if (otaUpTarget == 1) {
    if (Update.write(data, len) != len) { Update.printError(Serial); otaUpErr = true; }
  } else if (otaUpTarget == 2) {
    if (otaUpFsFile) otaUpFsFile.write(data, len);
  }
  if (final) {
    if (otaUpTarget == 1) { if (!Update.end(true)) { Update.printError(Serial); otaUpErr = true; } }
    else if (otaUpTarget == 2) { if (otaUpFsFile) otaUpFsFile.close(); }
    otaUpTarget = 0;
  }
}

void SetupAsyncServer() {
  Serial.println("SetupAsyncServer()\n");
  for(int i=0; i<7; i++) { bootid[i]=random(26)+'A'; }
  bootid[7] = 0;
  defaultUserLevel = getDefaultAuthLevel();
  server.reset();
  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/index.html", String(), false, processor);
  });

  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/index.html", String(), false, processor);
  });

  server.on("/qrg.json", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 1)) return;
    request->send(200, "text/html", getQRGAsJson());
  });

  server.on("/qrg.html", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 1)) return;
    request->send(200, "text/html", createQRGForm(reqAuthLevel(request)));
  });

  server.on("/qrg.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    handleQRGPost(request);
    request->send(200, "text/html", createQRGForm(2));
  });

  server.on("/wifi.html", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    request->send(200, "text/html", createWIFIForm());
  });
  
  server.on("/wifi.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    handleWIFIPost(request);
    request->send(200, "text/html", createWIFIForm());
  });

  server.on("/config.html", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 1)) return;   // level 1 may view config; saving (POST) needs level 2
    request->send(200, "text/html", createConfigForm(reqAuthLevel(request)));
  });

  server.on("/config.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    handleConfigPost(request);
    request->send(200, "text/html", createConfigForm(2));
  });

  server.on("/status.html", HTTP_GET,  [](AsyncWebServerRequest * request) {
    request->send(200, "text/html", createStatusForm());
  });
  server.on("/live.json", HTTP_GET,  [](AsyncWebServerRequest * request) {
    request->send(200, "text/json", createLiveJson());
  });
  server.on("/spectrum.json", HTTP_GET,  [](AsyncWebServerRequest * request) {
    request->send(200, "text/json", createSpectrumJson());
  });
  server.on("/livemap.html", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/livemap.html", String(), false, processor);
  });
  server.on("/livemap.js", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/livemap.js", String(), false, processor);
  });
  server.on("/update.html", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;   // firmware update is an admin action
    request->send(200, "text/html", createUpdateForm(0));
  });
  server.on("/update.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    handleUpdatePost(request);
    request->send(200, "text/html", createUpdateForm(1));
  });

  server.on("/control.html", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 1)) return;   // level 1 may view and use the control tab
    request->send(200, "text/html", createControlForm(reqAuthLevel(request), false));
  });
  server.on("/control.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 1)) return;   // control actions (rx/scan/spectrum/...) allowed at level 1
    int lvl = reqAuthLevel(request);
    bool screenChanged = handleControlPost(request, lvl);
    request->send(200, "text/html", createControlForm(lvl, screenChanged));
  });

  server.on("/login.html", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/login.html", String(), false, processor);
  });
  server.on("/login.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    handleLoginPost(request);
  });
  server.on("/logout", HTTP_GET, [](AsyncWebServerRequest * request) {
    handleLogout(request);
  });

  server.on("/sdfiles.html", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(LittleFS, "/sdfiles.html", String(), false, processor);
  });

  server.on("/users.html", HTTP_GET, [](AsyncWebServerRequest * request) {
    if(!isUserMgmtAllowed(request)) return;
    request->send(LittleFS, "/users.html", String(), false, processor);
  });
  server.on("/users.json", HTTP_GET, [](AsyncWebServerRequest * request) {
    if(!isUserMgmtAllowed(request)) return;
    getUserListJson(message, sizeof(message));
    request->send(200, "application/json", message);
  });
  server.on("/users.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isUserMgmtAllowed(request)) return;
    handleUsersPost(request);
  });
  // Effective access level for the current request (max of the open default level and the
  // session level -- same rule as isAuthenticated). Used by index.html to hide nav tabs the
  // user cannot access. Purely cosmetic: every route still enforces auth server-side.
  server.on("/whoami.json", HTTP_GET, [](AsyncWebServerRequest * request) {
    char session[COOKIE_SIZE];
    getSessionCookie(request, session, COOKIE_SIZE);
    int level = defaultUserLevel;
    if(session[0]) {
      int slvl = getCookieAuthLevel(session);
      if(slvl > level) level = slvl;
    }
    char user[USERLEN+1] = "";
    if(session[0]) {
      int i = 0;
      for(; i < USERLEN && session[i] && session[i] != ':'; i++) user[i] = session[i];
      user[i] = 0;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"user\":\"%s\",\"level\":%d}", user, level);
    request->send(200, "application/json", buf);
  });

  server.on("/file", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    String url = request->url();
    const char *filename = url.c_str() + 5;
    if (*filename == 0) {
      request->send(400, "error");
      return;
    }
    request->send(LittleFS, filename, "text/plain");
  });
  
  server.on("/file", HTTP_POST,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    request->send(200);
  }, handleUpload);
#if FEATURE_SDCARD
  server.serveStatic("/sd/", SD, "/");
  //server.on("/sd/files.json", HTTP_GET, [](AsyncWebServerRequest *request) {  } ); /// TODO: fix later, temporarily keep for bkward compat

  server.on("/files.json", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(!isAuthenticated(request, 2)) return;
#define FILES_JSON_MAX_SIZE 4096
#define FILES_JSON_MAX_ENTRY 128
    String subdir;
    if(request->hasParam("dir")) {
      String dirParam = request->getParam("dir")->value();
      if(dirParam.indexOf("..") >= 0 || dirParam.length() > 32) { request->send(400, "application/json", "[]"); return; }
      if(dirParam == ".int") {
        subdir = "/littlefs/";  /* magic: list LittleFS root */
      } else {
        subdir = "/sd/" + dirParam + "/";
      }
    } else {
      subdir = "/sd/";
    }
    int start = 0;
    if(request->hasParam("start")) {
      start = request->getParam("start")->value().toInt();
      if(start < 0) start = 0;
    }
    int len = 0;
    DIR *dir = NULL;
    const size_t bodyCap = FILES_JSON_MAX_SIZE - 2;
    xSemaphoreTake(globalLock, portMAX_DELAY);
    dir = opendir(subdir.c_str());
    xSemaphoreGive(globalLock);
    if(!dir) {
      request->send(500, "application/json", "[]");
      return;
    }
    len = snprintf(message, FILES_JSON_MAX_SIZE, "[");
    if(len < 0 || (size_t)len >= bodyCap) { xSemaphoreTake(globalLock, portMAX_DELAY); closedir(dir); xSemaphoreGive(globalLock); request->send(500, "application/json", "[]"); return; }
    int index = 0;
    for(;;) {
      char d_name_copy[64];
      int is_dir = 0;
      time_t mtime = 0;
      long fsize = 0;
      int got = 0;
      xSemaphoreTake(globalLock, portMAX_DELAY);
      struct dirent *dent = readdir(dir);
      if(!dent) {
        closedir(dir);
        xSemaphoreGive(globalLock);
        break;
      }
      if(strcmp(dent->d_name, ".") == 0 || strcmp(dent->d_name, "..") == 0) {
        xSemaphoreGive(globalLock);
        continue;
      }
      strncpy(d_name_copy, dent->d_name, sizeof(d_name_copy) - 1);
      d_name_copy[sizeof(d_name_copy) - 1] = '\0';
      if(index < start) {
        index++;
        xSemaphoreGive(globalLock);
        continue;
      }
      char fname[128];
      struct stat attr;
      snprintf(fname, sizeof(fname), "%s%s", subdir.c_str(), dent->d_name);
      if(stat(fname, &attr) == 0) {
        is_dir = S_ISDIR(attr.st_mode) ? 1 : 0;
        mtime = attr.st_mtime;
        fsize = (long)attr.st_size;
        got = 1;
      }
      xSemaphoreGive(globalLock);
      if(!got) continue;
      if((size_t)(len + FILES_JSON_MAX_ENTRY) >= bodyCap) {
        xSemaphoreTake(globalLock, portMAX_DELAY);
        closedir(dir);
        xSemaphoreGive(globalLock);
        break;
      }
      char ftim[50];
      strftime(ftim, sizeof(ftim), "%Y-%m-%dT%H:%M:%SZ", gmtime(&mtime));
      int n;
      if(is_dir)
        n = snprintf(message + len, (size_t)(FILES_JSON_MAX_SIZE - len), "%s{\"name\":\"%s\",\"dir\":1,\"ts\":\"%s\"}", len > 1 ? "," : "", d_name_copy, ftim);
      else
        n = snprintf(message + len, (size_t)(FILES_JSON_MAX_SIZE - len), "%s{\"name\":\"%s\",\"size\":%ld,\"ts\":\"%s\"}", len > 1 ? "," : "", d_name_copy, fsize, ftim);
      if(n < 0 || (size_t)(len + n) >= bodyCap) {
        xSemaphoreTake(globalLock, portMAX_DELAY);
        closedir(dir);
        xSemaphoreGive(globalLock);
        break;
      }
      len += n;
      index++;
    }
    len += snprintf(message + len, (size_t)(FILES_JSON_MAX_SIZE - len), "]");
    if(len < 0 || (size_t)len >= FILES_JSON_MAX_SIZE) { request->send(500, "application/json", "[]"); return; }
    message[len] = '\0';
    request->send(200, "application/json", message);
#undef FILES_JSON_MAX_ENTRY
#undef FILES_JSON_MAX_SIZE
  });
  server.on("/sdrm.html", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(!isAuthenticated(request, 2)) return;
    const AsyncWebParameter *param = request->getParam(0);
    if(!param) { request->send(404); return; }
    String path = param->value();
    if(path.indexOf("..") >= 0 || path.length() > 80) { request->send(400, "text/html", "<html><body>invalid path</body></html>"); return; }
    char filename[96];
    snprintf(filename, sizeof(filename), "/%s", path.c_str());
    File f = SD.open(filename);
    if(!f) {
      request->send(404, "text/html", "<html><body>not found</body></html>");
      return;
    }
    bool isDir = f.isDirectory();
    f.close();
    if(isDir) {
      char vfsPath[96];
      snprintf(vfsPath, sizeof(vfsPath), "/sd/%s", path.c_str());
      if(deleteSdDirRecursive(filename, vfsPath)) {
        request->send(200, "text/html", "<html><body>ok</body></html>");
      } else {
        request->send(500, "text/html", "<html><body>failed to delete folder</body></html>");
      }
    } else {
      if(SD.remove(filename)) {
        request->send(200, "text/html", "<html><body>ok</body></html>");
      } else {
        char info[256];
        snprintf(info, sizeof(info), "<html><body>failed to delete '%s'\n</body></html>", filename);
        request->send(404, "text/html", info);
      }
    }
  });
#endif

  server.on("/edit.html", HTTP_GET,  [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    // new version:
    // Open file
    // store file object in request->_tempObject
    //request->send(200, "text/html", createEditForm(request->getParam(0)->value()));
    const AsyncWebParameter *param = request->getParam(0);
    if(!param) {
      request->send(404);
      return;
    }
    const String filename = param->value();
    File file = LittleFS.open("/" + filename, "r");
    int state = 0;
    request->send("text/html", 0, [state, file, filename](uint8_t *buffer, size_t maxLen, size_t index) mutable -> size_t  {
      LOG_D(TAG, "******* send callback: %d %d %d\n", state, maxLen, index);
      return streamEditForm(state, file, filename, (char *)buffer, maxLen, index);
    });
  });
  server.on("/edit.html", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;
    const char *ret = handleEditPost(request);
    if (ret == NULL)
      request->send(200, "text/html", "<html><head>ERROR</head><body><p>Something went wrong (probably ESP32 out of memory). Uploaded file is empty.</p></body></hhtml>");
    else {
      const AsyncWebParameter *param = request->getParam(0);
      if(!param) {
         request->send(404);
         return;
      }
      String f = param->value();
      request->redirect("/edit.html?file=" + f);
    }
  },
  NULL,
  [](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    LOG_D(TAG, "post data: index=%d len=%d total=%d\n", index, len, total);
  });

  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest * request) {
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/style.css", "text/css");
    if(response) {
      response->addHeader("Cache-Control", "max-age=86400");
      request->send(response);
    } else {
      request->send(404);
    }
  });

  server.on("/live.kml", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "application/vnd.google-earth.kml+xml", createKMLLive(sonde.ipaddr.c_str()));
  });

  server.on("/dynamic.kml", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "application/vnd.google-earth.kml+xml", createKMLDynamic());
  });

  server.on("/upd.html", HTTP_GET, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;   // update check/trigger page is admin-only
    request->send(LittleFS, "/upd.html", String(), false, processor);
  });

  // Current boot nonce; the update page polls this to detect when the device has
  // finished flashing and rebooted (the bootid changes on each boot). Unauthenticated
  // on purpose: it carries no secret and must stay reachable across the reboot.
  server.on("/bootid", HTTP_GET, [](AsyncWebServerRequest * request) {
    request->send(200, "text/plain", bootid);
  });

  // Upload firmware (update.ino.bin) and/or filesystem (update.fs.bin) directly from the
  // browser. Gated by the hidden allowfileupload config option; admin-only. handleOtaUpload
  // streams the parts; this completion handler unpacks any filesystem archive and reboots.
  server.on("/uploadota", HTTP_POST, [](AsyncWebServerRequest * request) {
    if(!isAuthenticated(request, 2)) return;   // admin-only (sends login redirect if not)
    bool enabled = sonde.config.allowfileupload;
    bool ok = enabled && otaUpInProgress && !otaUpErr && (otaUpFw || otaUpFs);
    if (ok && otaUpFs) {                        // unpack the buffered filesystem archive
      File f = LittleFS.open(OTA_FS_TMP, "r");
      if (!f || unpackFsArchive(f) < 0) ok = false;
      if (f) f.close();
    }
    if (otaUpFs) LittleFS.remove(OTA_FS_TMP);
    otaUpInProgress = false;
    const char *msg = !enabled ? "File upload is disabled (set allowfileupload=1)."
                    : ok       ? "Update applied. Rebooting..."
                               : "Update failed. See the serial log.";
    request->send(enabled ? (ok ? 200 : 500) : 403, "text/plain", msg);
    if (ok) otaRebootAt = millis() + 1500;      // reboot after the response is delivered
  }, handleOtaUpload);

  server.on("/status.json", HTTP_GET, [](AsyncWebServerRequest * request) {
   int nr = 0;
   AsyncWebServerResponse *response = request->beginChunkedResponse("application/json", [nr](uint8_t *buf, size_t maxLen, size_t index) mutable-> size_t {
       if(connectors[nr]==NULL) return 0;
       if(index==0) { strcpy((char *)buf, "{"); buf++; maxLen--; } else *buf=0;
       snprintf( (char *)buf, maxLen-2, "\"%s\": \"%s\"\n", connectors[nr]->getName().c_str(), connectors[nr]->getStatus().c_str() );
       nr++;
       strcat((char *)buf, connectors[nr]==NULL ? "}\n":",\n");
       return strlen((char *)buf);
     }); 
     request->send(response);
  });

  server.onNotFound([](AsyncWebServerRequest * request) {
    if (request->method() == HTTP_OPTIONS) {
      request->send(200);
    } else {
      String url = request->url();
      // Never serve credential files to unauthenticated clients (this static fallback
      // would otherwise expose user.txt / networks.txt / config.txt by direct URL).
      if (isSensitiveFile(url.c_str())) {
        if(!isAuthenticated(request, 2)) return;
      }
      // Any other non-asset file (config/data such as qrg.txt, screens*.txt, gpsinit.txt,
      // GPSRESET) is private: require at least a logged-in (level 1) session before serving
      // it by direct URL. Public web assets (css/js/html/images/...) stay open.
      else if (!isPublicStaticAsset(url)) {
        if(!isAuthenticated(request, 1)) return;
      }
      if (url.endsWith(".gpx"))
        request->send(200, "application/gpx+xml", sendGPX(request));
      else {
        // TODO: set correct type for .js

        // Caching is an important work-around for a bug somewhere in the network stack that causes corrupt replies
        // with platform-espressif32 (some TCP segments simply get lost before being sent, so reply header and parts of data is missing)
        // This happens with concurrent requests, notably if a browser fetches rdz.js and cfg.js concurrently for config.html
        // With the cache, rdz.js is likely already in the cache...0
        LOG_D(TAG, "URL is %s\n", url.c_str());
	const char *type = "text/html";
        if(url.endsWith(".js")) type="text/javascript";
        LOG_D(TAG, "Responding with type %s (url %s)\n", type, url.c_str());
        AsyncWebServerResponse *response = request->beginResponse(LittleFS, url, type);
        if(response) {
          response->addHeader("Cache-Control", "max-age=900"); 
          request->send(response);
        } else {
          request->send(404);
        }
      }
    }
  });

  // Start server
  server.begin();
}

int fetchWifiIndex(const char *id) {
  for (int i = 0; i < nNetworks; i++) {
    if (strcmp(id, networks[i].id.c_str()) == 0) {
      LOG_D(TAG, "Match for %s at %d\n", id, i);
      return i;
    }
    //LOG_D(TAG, "No match: '%s' vs '%s'\n", id, networks[i].id.c_str());
  }
  return -1;
}

const char *fetchWifiSSID(int i) {
  return networks[i].id.c_str();
}
const char *fetchWifiPw(int i) {
  return networks[i].pw.c_str();
}

const char *fetchWifiPw(const char *id) {
  for (int i = 0; i < nNetworks; i++) {
    //Serial.print("Comparing '");
    //Serial.print(id);
    //Serial.print("' and '");
    //Serial.print(networks[i].id.c_str());
    //Serial.println("'");
    if (strcmp(id, networks[i].id.c_str()) == 0) return networks[i].pw.c_str();
  }
  return NULL;
}

// It is not safe to call millis() in ISR!!!
// millis() does a division int64_t by 1000 for which gcc creates a library call
// on a 32bit system, and the called function has no IRAM_ATTR
// so doing it manually...
// Code adapted for 64 bits from https://www.hackersdelight.org/divcMore.pdf
static int64_t IRAM_ATTR divs10(int64_t n) {
  int64_t q, r;
  n = n + (n >> 63 & 9);
  q = (n >> 1) + (n >> 2);
  q = q + (q >> 4);
  q = q + (q >> 8);
  q = q + (q >> 16);
  q = q + (q >> 32);
  q = q >> 3;
  r = n - q * 10;
  return q + ((r + 6) >> 4);
  // return q + (r > 9);
}

static int64_t IRAM_ATTR divs1000(int64_t n) {
  return divs10(divs10(divs10(n)));
}

static unsigned long IRAM_ATTR my_millis()
{
  return divs1000(esp_timer_get_time());
}

static void checkTouchStatus();
static void touchISR();
static void touchISR2();

// ISR won't work for SPI transfer, so forget about the following approach
///// Also initialized timers for sx1278 handling with interruts
///// fastest mode currentily is 4800 bit/s, i.e. 600 bytes/sec
///// 64 byte FIFO will last for at most about 106 ms.
///// lets use a timer every 20ms to handle sx1278 FIFO input, that should be fine.
// Instead create a tast...

Ticker ticker;
Ticker ledFlasher;

#define IS_TOUCH(x) (((x)!=255)&&((x)!=-1)&&((x)&128))
void initTouch() {
  // also used for LED
  ticker.attach_ms(300, checkTouchStatus);

  if ( !(IS_TOUCH(sonde.config.button_pin) || IS_TOUCH(sonde.config.button2_pin)) ) return; // no touch buttons configured
  /*
   *  ** no. readTouch is not safe to use in ISR!
      so now using Ticker
    hw_timer_t *timer = timerBegin(0, 80, true);
    timerAttachInterrupt(timer, checkTouchStatus, true);
    timerAlarmWrite(timer, 300000, true);
    timerAlarmEnable(timer);
  */

  if ( IS_TOUCH(sonde.config.button_pin) ) {
    touchAttachInterrupt(sonde.config.button_pin & 0x7f, touchISR, sonde.config.touch_thresh);
    LOG_I(TAG, "Initializing touch 1 on pin %d\n", sonde.config.button_pin & 0x7f);
  }
  if ( IS_TOUCH(sonde.config.button2_pin) ) {
    touchAttachInterrupt(sonde.config.button2_pin & 0x7f, touchISR2, sonde.config.touch_thresh);
    LOG_I(TAG, "Initializing touch 2 on pin %d\n", sonde.config.button2_pin & 0x7f);
  }
}



const char *getStateStr(int what) {
  if (what < 0 || what >= (sizeof(mainStateStr) / sizeof(const char *)))
    return "--";
  else
    return mainStateStr[what];
}

// Web scan plot: when idle (no sonde locked) and the configured interval has
// elapsed, run one data-only spectrum sweep, then restore decode tuning.
static unsigned long lastScanPlotMillis = 0;
// A watcher is considered present if /spectrum.json was polled within this
// window. Must exceed the page's poll interval (POLL_MS=3000 in scanplot.html)
// with margin so a single missed/slow poll doesn't drop the watcher.
static const unsigned long SCANPLOT_WATCH_MS = 10000;
static void maybeScanPlotSweep() {
  if (sonde.config.scanplotint <= 0) return;          // feature disabled
  SondeInfo *si = &sonde.sondeList[sonde.currentSonde];
  if (si->lastState == 1) return;                     // locked onto a sonde -> never sweep
  unsigned long now = millis();
  // Only sweep while a browser is actually viewing the scan plot (it polls
  // /spectrum.json every few seconds). With nobody watching, skip the sweep
  // entirely so the radio stays fully available to the sonde search.
  if (lastSpectrumPollMs == 0 || (now - lastSpectrumPollMs) > SCANPLOT_WATCH_MS) return;
  // A watcher is present. If no scan exists in memory yet (seq still 0), sweep
  // immediately so the page gets a plot without waiting; otherwise throttle to
  // the configured interval.
  if (scanner.webSeq() != 0) {
    unsigned long interval = (unsigned long)sonde.config.scanplotint * 1000UL;
    if ((now - lastScanPlotMillis) < interval) return;
  }
  lastScanPlotMillis = now;
  LOG_I(TAG, "ScanPlot: idle sweep\n");
  scanner.scanForWeb();                               // data-only sweep, bumps seq
  sonde.setup();                                      // restore radio tuning for current sonde
}

void sx1278Task(void *parameter) {
  /* new strategy:
      background tasks handles all interactions with sx1278.
      implementation is decoder specific.
      This task is a simple infinit loop that
       (a) initially and after frequency or mode change calls <decoder>.setup()
       (b) then repeatedly calls <decoder>.receive() which should
           (1) update data in the Sonde structure (additional updates may be done later in main loop/waitRXcomplete)
           (2) set output flag receiveResult (success/error/timeout and keybord events)

  */
  while (1) {
    if (rxtask.activate >= 128) {
      // activating sx1278 background task...
      LOG_I(TAG, "RXtask: start DECODER for sonde %d (was %s)\n", rxtask.activate & 0x7f, getStateStr(rxtask.mainState));
      rxtask.mainState = ST_DECODER;
      rxtask.currentSonde = rxtask.activate & 0x7F;
      sonde.setup();
    } else if (rxtask.activate != -1) {
      LOG_I(TAG, "RXtask: start %s (was %s)\n", getStateStr(rxtask.activate), getStateStr(rxtask.mainState));
      rxtask.mainState = rxtask.activate;
    }
    rxtask.activate = -1;
    /* only if mainState is ST_DECODER */
    if (rxtask.mainState != ST_DECODER) {
      delay(100);
      continue;
    }
    sonde.receive();
    maybeScanPlotSweep();
    delay(20);
  }
}


static void IRAM_ATTR touchISR() {
  if (!button1.isTouched) {
    unsigned long now = my_millis();
    if (now - button1.keydownTime < 500) button1.doublepress = 1;
    else button1.doublepress = 0;
    button1.keydownTime = now;
    button1.isTouched = true;
  }
}

static void IRAM_ATTR touchISR2() {
  if (!button2.isTouched) {
    unsigned long now = my_millis();
    if (now - button2.keydownTime < 500) button2.doublepress = 1;
    else button2.doublepress = 0;
    button2.keydownTime = now;
    button2.isTouched = true;
  }
}

// touchRead in ISR is also a bad idea. Now moved to Ticker task
static void checkTouchButton(Button & button) {
  if (button.isTouched) {
    int tmp = touchRead(button.pin & 0x7f);
    //LOG_D(TAG, "touch read %d: value is %d\n", button.pin & 0x7f, tmp);
    if (tmp > sonde.config.touch_thresh + 5) {
      button.isTouched = false;
      unsigned long elapsed = my_millis() - button.keydownTime;
      if (elapsed > 1500) {
        if (elapsed < 4000) {
          button.pressed = KP_MID;
        }
        else {
          button.pressed = KP_LONG;
        }
      } else if (button.doublepress) {
        button.pressed = KP_DOUBLE;
      } else {
        button.pressed = KP_SHORT;
      }
    }
  }
}

static unsigned long t_muted = (unsigned long)-1;

void ledOffCallback() {
  digitalWrite(sonde.config.led_pout, LOW);
}
void flashLed(int ms) {
  if (sonde.config.led_pout >= 0) {
    if(t_muted != -1) {
      LOG_D(TAG, "Muted at %d\n", t_muted);
      // t_muted was set by key press to mute LED / buzzer
      if(millis()-t_muted < sonde.config.b2mute * 60000L) return;
      else t_muted = -1;
      LOG_D(TAG, "Unmuted\n");
    }
    Serial.println("Not muted");
    digitalWrite(sonde.config.led_pout, HIGH);
    ledFlasher.once_ms(ms, ledOffCallback);
  }
}

int doTouch = 0;
static void checkTouchStatus() {
  checkTouchButton(button1);
  checkTouchButton(button2);
}

unsigned long bdd1, bdd2;
static bool b1wasdown = false;
static void IRAM_ATTR buttonISR() {
  if (digitalRead(button1.pin) == 0) { // Button down
    b1wasdown = true;
    unsigned long now = my_millis();
    if (now - button1.keydownTime < 500) {
      // Double press
      if (now - button1.keydownTime > 100)
        button1.doublepress = 1;
      bdd1 = now; bdd2 = button1.keydownTime;
    } else {
      button1.doublepress = 0;
    }
    button1.numberKeyPresses += 1;
    button1.keydownTime = now;
  } else { //Button up
    if (!b1wasdown) return;
    b1wasdown = false;
    unsigned long now = my_millis();
    if (button1.doublepress == -1) return;   // key was never pressed before, ignore button up
    unsigned int elapsed = now - button1.keydownTime;
    if (elapsed > 1500) {
      if (elapsed < 4000) {
        button1.pressed = KP_MID;
      }
      else {
        button1.pressed = KP_LONG;
      }
    } else {
      if (button1.doublepress) button1.pressed = KP_DOUBLE;
      else button1.pressed = KP_SHORT;
    }
    button1.numberKeyPresses += 1;
    button1.keydownTime = now;
  }
}

static void IRAM_ATTR button2ISR() {
  if (digitalRead(button2.pin) == 0) { // Button down
    unsigned long now = my_millis();
    if (now - button2.keydownTime < 500) {
      // Double press
      if (now - button2.keydownTime > 100)
        button2.doublepress = 1;
      //bdd1 = now; bdd2 = button1.keydownTime;
    } else {
      button2.doublepress = 0;
    }
    button2.numberKeyPresses += 1;
    button2.keydownTime = now;
  } else { //Button up
    unsigned long now = my_millis();
    if (button2.doublepress == -1) return;   // key was never pressed before, ignore button up
    unsigned int elapsed = now - button2.keydownTime;
    if (elapsed > 1500) {
      if (elapsed < 4000) {
        button2.pressed = KP_MID;
      }
      else {
        button2.pressed = KP_LONG;
      }
    } else {
      if (button2.doublepress) button2.pressed = KP_DOUBLE;
      else button2.pressed = KP_SHORT;
    }
    button2.numberKeyPresses += 1;
    button2.keydownTime = now;
  }
}

int getKeyPress() {
  KeyPress p = button1.pressed;
  button1.pressed = KP_NONE;
#if 0
  int x = digitalRead(button1.pin);
  LOG_D(TAG, "Debug: bdd1=%ld, bdd2=%ld\n", bdd1, bdd2);
  LOG_D(TAG, "button1 press (dbl:%d) (now:%d): %d at %ld (%d)\n", button1.doublepress, x, p, button1.keydownTime, button1.numberKeyPresses);
#endif
  return p;
}

// called by arduino main loop (from Sonde::waitRXcomplete) as soon as pmu_irq is set
void handlePMUirq() {
  if (sonde.config.button2_axp) {
    // Use AXP power button as second button
    int key = pmu->handleIRQ();
    if (key > 0) {
      button2.pressed = (KeyPress)key;
      button2.keydownTime = my_millis();
    }
  } else {
    // WiFi loop intentionally calls this in order to react to PMU key press
    //Serial.println("handlePMIirq() called. THIS SHOULD NOT HAPPEN w/o button2_axp set");
    pmu_irq = 0;   // prevent main loop blocking
  }
}

int getKey2Press() {
  // TODO: Should be atomic
  KeyPress p = button2.pressed;
  button2.pressed = KP_NONE;
  //LOG_D(TAG, "button2 press: %d at %ld (%d)\n", p, button2.keydownTime, button2.numberKeyPresses);
  return p;
}

int getKeyPressEvent() {
  int p = getKeyPress();
  if (p == KP_NONE) {
    p = getKey2Press();
    if (p == KP_NONE)
      return EVT_NONE;
    if (p == KP_RINEX)
      return EVT_RINEX;
    if (p == KP_FORMAT)
      return EVT_FORMAT;
    LOG_D(TAG, "Key 2 was pressed [%d]\n", p + 4);
    // maybe not the best place, but easy to do: check for B2 medium keypress to mute LED
    if(p == KP_MID && sonde.config.b2mute > 0) {
       if(t_muted==-1) t_muted = millis(); else t_muted = -1;
    }
    return p + 4;
  }
  LOG_D(TAG, "Key 1 was pressed [%d]\n", p);
  return p;  /* map KP_x to EVT_KEY1_x / EVT_KEY2_x*/
}

#define SSD1306_ADDRESS 0x3c
bool ssd1306_found = false;
bool axp_found = false;

int scanI2Cdevice(void)
{
  byte err, addr;
  int nDevices = 0;
  for (addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    err = Wire.endTransmission();
    if (err == 0) {
      Serial.print("I2C device found at address 0x");
      if (addr < 16)
        Serial.print("0");
      Serial.print(addr, HEX);
      Serial.println(" !");
      nDevices++;

      if (addr == SSD1306_ADDRESS) {
        ssd1306_found = true;
        Serial.println("ssd1306 display found");
      }
      if (addr == AXP192_SLAVE_ADDRESS) {  // Same for AXP2101
        axp_found = true;
        Serial.println("axp2101 PMU found");
      }
    } else if (err == 4) {
      Serial.print("Unknow error at address 0x");
      if (addr < 16)
        Serial.print("0");
      Serial.println(addr, HEX);
    }
  }
  if (nDevices == 0)
    Serial.println("No I2C devices found\n");
  else
    Serial.println("done\n");
  return nDevices;
}

extern int initlevels[40];

#ifdef ESP_MEM_DEBUG
typedef void (*esp_alloc_failed_hook_t) (size_t size, uint32_t caps, const char * function_name);
extern esp_err_t heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);

void heap_caps_alloc_failed_hook(size_t requested_size, uint32_t caps, const char *function_name)
{
  printf("%s was called but failed to allocate %d bytes with 0x%X capabilities. \n", function_name, requested_size, caps);
}
#endif

static void enableLocalUpdates() {
  localUpdates = NULL;
  File local = LittleFS.open("/localupd.txt", "r");
  if(local && local.available()) {
    localUpdates = strdup(readLine(local).c_str());
  } 
  LOG_I(TAG, "Local update server for development: %s\n", localUpdates?localUpdates:"<disabled>");
}

void setup()
{
  char buf[12];

  // Open serial communications and wait for port to open:
  Serial.begin(115200);
  Log.init();

  for (int i = 0; i < 39; i++) {
    int v = gpio_get_level((gpio_num_t)i);
    LOG_D(TAG, "%d:%d ", i, v);
  }
  LOG_I(TAG, "sizeof long is %d\n", sizeof(long));

#ifndef REMOVE_ALL_FOR_TESTING

  Serial.println("");
#ifdef ESP_MEM_DEBUG
  esp_err_t error = heap_caps_register_failed_alloc_callback(heap_caps_alloc_failed_hook);
#endif
  axpSemaphore = xSemaphoreCreateBinary();
  xSemaphoreGive(axpSemaphore);

  for (int i = 0; i < 39; i++) {
    LOG_D(TAG, "%d:%d ", i, initlevels[i]);
  }
  Serial.println(" (before setup)");
  sonde.defaultConfig();  // including autoconfiguration

  delay(1000);
  Serial.println("Initializing LittleFS");
  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  // If the device has no password key yet (fresh device, or NVS was erased), any existing
  // /user.txt cannot be valid against it (it was plaintext from old firmware, or hashed
  // with a key that's gone). Wipe it so the admin re-bootstraps user accounts.
  if (!deviceKeysExist()) {
    Serial.println("No device key present: clearing /user.txt for fresh user setup");
    LittleFS.remove("/user.txt");
  }

  Serial.println("Reading initial configuration");
  setupConfigData();    // configuration must be read first due to OLED ports!!!
  frameCache.begin(sonde.config.cachesize);
  WiFi.setHostname(sonde.config.mdnsname);
  //WiFi.enableIPv6();

  // NOT TTGO v1 (fingerprint 64) or Heltec v1/v2 board (fingerprint 4)
  // and NOT TTGO Lora32 v2.1_1.6 (fingerprint 31/63)
  if ( ( (sonde.fingerprint & (64 + 31)) != 31) && ((sonde.fingerprint & 16) == 16) ) {
    // FOr T-Beam 1.0
    for (int i = 0; i < 2; i++) { // try multiple times
      Wire.begin(21, 22);
      // Make sure the whole thing powers up!?!?!?!?!?
      U8X8 *u8x8 = new U8X8_SSD1306_128X64_NONAME_HW_I2C(0, 22, 21);
      u8x8->initDisplay();
      delay(100);

      scanI2Cdevice();

      if (!pmu) {
        pmu = PMU::getInstance(Wire);
        if (pmu) {
          Serial.println("PMU found");
          pmu->init();
          if (sonde.config.button2_axp ) {
            //axp.enableIRQ(AXP202_VBUS_REMOVED_IRQ | AXP202_VBUS_CONNECT_IRQ | AXP202_BATT_REMOVED_IRQ | AXP202_BATT_CONNECT_IRQ, 1);
            //axp.enableIRQ( AXP202_PEK_LONGPRESS_IRQ | AXP202_PEK_SHORTPRESS_IRQ, 1 );
            //axp.clearIRQ();
            pmu->disableAllIRQ();
            pmu->enableIRQ();
          }
          int ndevices = scanI2Cdevice();
          if (sonde.fingerprint != 17 || ndevices > 0) break; // only retry for fingerprint 17 (startup problems of new t-beam with oled)
          delay(100);
        }
      }
    }
  }
  if (sonde.config.batt_adc >= 0) {
    pinMode(sonde.config.batt_adc, INPUT);
  }
  if (sonde.config.power_pout >= 0) { // for a heltec v2, pull GPIO21 low for display power
    pinMode(sonde.config.power_pout & 127, OUTPUT);
    digitalWrite(sonde.config.power_pout & 127, sonde.config.power_pout & 128 ? 1 : 0);
  }

  if (sonde.config.led_pout >= 0) {
    pinMode(sonde.config.led_pout, OUTPUT);
    flashLed(1000); // testing
  }

  button1.pin = sonde.config.button_pin;
  button2.pin = sonde.config.button2_pin;
  if (button1.pin != 0xff) {
    if ( (button1.pin & 0x80) == 0 && button1.pin < 34 ) {
      Serial.println("Button 1 configured as input with pullup");
      pinMode(button1.pin, INPUT_PULLUP);
    } else
      pinMode(button1.pin, INPUT);  // configure as input if not disabled
  }
  if (button2.pin != 0xff) {
    if ( (button2.pin & 0x80) == 0 && button2.pin < 34 ) {
      Serial.println("Button 2 configured as input with pullup");
      pinMode(button2.pin, INPUT_PULLUP);
    } else
      pinMode(button2.pin, INPUT);  // configure as input if not disabled
  }
  // Handle button press
  if ( (button1.pin & 0x80) == 0) {
    attachInterrupt( button1.pin, buttonISR, CHANGE);
    LOG_D(TAG, "button1.pin is %d, attaching interrupt\n", button1.pin);
  }
  // Handle button press
  if ( (button2.pin & 0x80) == 0) {
    attachInterrupt( button2.pin, button2ISR, CHANGE);
    LOG_D(TAG, "button2.pin is %d, attaching interrupt\n", button2.pin);
  }
  initTouch();

  disp.init();
  delay(100);
  Serial.println("Showing welcome display");
  disp.rdis->welcome();
  delay(3000);
  Serial.println("Clearing display");
  sonde.clearDisplay();

  setupWifiList();
  LOG_D(TAG, "before disp.initFromFile... layouts is %p\n", disp.layouts);
  disp.initFromFile(sonde.config.screenfile);
  LOG_D(TAG, "disp.initFromFile... layouts is %p\n", disp.layouts);


#if 0
  if (1) { //testing cyd
    digitalWrite(sonde.config.sx1278_ss, HIGH);
    pinMode(sonde.config.sx1278_ss, OUTPUT);

    SPI.begin(18, 19, 23, -1);    // CKL, MISO, MOSI, CS)
  } else 
  if (sonde.config.type == TYPE_M5_CORE2) {
    // Core2 uses Pin 38 for MISO
    SPI.begin(18, 38, 23, -1);
  } else if (sonde.config.type == TYPE_M5_CORE) {
    SPI.begin(18, 19, 23, -1);
    // GPIO26 is reset
    pinMode(26, OUTPUT);
    digitalWrite(26, 0);
    delay(5);
    digitalWrite(26, 1);
    delay(5);
  } else {
    SPI.begin();
  }
  //Set most significant bit first
  SPI.setBitOrder(MSBFIRST);
  //Divide the clock frequency
  SPI.setClockDivider(SPI_CLOCK_DIV2);
  //Set data mode
  SPI.setDataMode(SPI_MODE0);
#endif
#if 1

  sx1278.setup(globalLock);

  int i = 0;
  while (++i < 10) {
    // == check the radio chip by setting default frequency =========== //
    sx1278.ON();
    if (sx1278.setFrequency(402700000) == 0) {
      Serial.println(F("Setting freq: SUCCESS "));
    } else {
      Serial.println(F("Setting freq: ERROR "));
    }
    float f = sx1278.getFrequency();
    LOG_D(TAG, "Frequency set to %d", f);
    // == check the radio chip by setting default frequency =========== //
    if( f>402700000-1000 && f<402700000+1000 ) break;
    delay(500);
  }
#endif

  //sx1278.setLNAGain(-48);
  sx1278.setLNAGain(0);

  // Optionally enable LnaBoostHf (RegLna 0x0C bits 1-0 = 0b11 -> 150% LNA current).
  // AGC (enabled for some sonde types) only controls the LnaGain field (bits 7-5),
  // it leaves these boost bits untouched, so setting it once here is sufficient.
  // Mainly useful when no external LNA is present (config option "lnaboost").
  if (sonde.config.lnaboost) {
    uint8_t lna = sx1278.readRegister(REG_LNA);
    sx1278.writeRegister(REG_LNA, lna | 0x03);
  }

  int gain = sx1278.getLNAGain();
  Serial.print("RX LNA Gain is ");
  Serial.println(gain);

  // Print a success message
  Serial.println(F("SX1278 configuration finished"));

  Serial.println("Setup finished");
  Serial.println();
  // int returnValue = pthread_create(&wifithread, NULL, wifiloop, (void *)0);

  //  if (returnValue) {
  //     Serial.println("An error has occurred");
  //  }
  //   xTaskCreate(mainloop, "MainServer", 10240, NULL, 10, NULL);


  // == setup default channel list if qrg.txt read fails =========== //
  sonde.clearSonde();
  setupChannelList();
  /// not here, done by sonde.setup(): rs41.setup();
  // == setup default channel list if qrg.txt read fails =========== //
#ifndef DISABLE_SX1278
  xTaskCreate( sx1278Task, "sx1278Task",
               10000, /* stack size */
               NULL, /* paramter */
               1, /* priority */
               NULL);  /* task handle*/
#endif
  sonde.setup();
  fixedToPosInfo();
  initGPS();
#if FEATURE_APRS
  connAPRS.init();
#endif
#if FEATURE_SDCARD
  connSDCard.init();
#endif
#if FEATURE_SERIALOUT
  connSerialOut.init();
#endif

  enableLocalUpdates();   // check if local updates from other servers is allowed

  WiFi.onEvent(WiFiEvent);
  getKeyPress();    // clear key buffer

#else
/// DEBUG ONLY
  WiFi.begin("Dinosauro", "03071975");
  while(WiFi.status() != WL_CONNECTED) { delay(500); Serial.print(":"); }
  Serial.println("... WiFi is connected!\n");
  SetupAsyncServer();
  sonde.config.sd.cs = 13;
  sonde.config.sd.clk = 14;
  sonde.config.sd.miso = 2;
  sonde.config.sd.mosi = 15;
  connSDCard.init();
#endif
}

void enterMode(int mode, bool force) {
  LOG_D(TAG, "enterMode(%d)\n", mode);
  // Auto-scan: when enabled, "start decoding" means "start searching the spectrum
  // for peaks". force=true bypasses this and is used by the auto-scan loop itself
  // to lock onto a found sonde.
  if (mode == ST_DECODER && !force && autoscanActive()) {
    mode = ST_AUTOSCAN;
  }
  // Backround RX task should only be active in mode ST_DECODER for now
  // (future changes might use RX background task for spectrum display as well)
  if (mode != ST_DECODER) {
    rxtask.activate = mode;
    while (rxtask.activate == mode) {
      delay(10);  // until cleared by RXtask -- rx task is deactivated
    }
  }
  mainState = (MainState)mode;
  if (mainState == ST_SPECTRUM) {
    Serial.println("Entering ST_SPECTRUM mode");
    sonde.clearDisplay();
    disp.rdis->setFont(FONT_SMALL);
    specTimer = millis();
    //scanner.init();
  } else if (mainState == ST_AUTOSCAN) {
    Serial.println("Entering ST_AUTOSCAN mode");
    sonde.clearDisplay();
    disp.rdis->setFont(FONT_SMALL);
    autoscanReset();
  } else if (mainState == ST_WIFISCAN || mainState == ST_RINEX_UPDATE || mainState == ST_FORMAT_SD) {
    sonde.clearDisplay();
  }

  if (mode == ST_DECODER) {
    // trigger activation of background task
    // currentSonde should be set before enterMode()
    rxtask.activate = ACT_SONDE(sonde.currentSonde);
    //
    Serial.println("clearing and updating display");
    sonde.clearDisplay();
    sonde.updateDisplay();
  }
  printf("enterMode ok\n");
}

static char text[40];
static const char *action2text(uint8_t action) {
  if (action == ACT_DISPLAY_DEFAULT) return "Default Display";
  if (action == ACT_DISPLAY_SPECTRUM) return "Spectrum Display";
  if (action == ACT_DISPLAY_WIFI) return "Wifi Scan Display";
  if (action == ACT_NEXTSONDE) return "Go to next sonde";
  if (action == ACT_PREVSONDE) return "presonde (not implemented)";
  if (action == ACT_RINEX_UPDATE) return "update RINEX eph data";
  if (action == ACT_FORMAT_SD) return "format SD card";
  if (action == ACT_NONE) return "none";
  if (action >= 128) {
    snprintf(text, 40, "Sonde=%d", action & 127);
  } else {
    snprintf(text, 40, "Display=%d", action);
  }
  return text;
}

#define RDZ_DATA_LEN 128
static char rdzData[RDZ_DATA_LEN];
static int rdzDataPos = 0;

#define REPLAY_PACE 4            // max backlog frames delivered per connector per tick
#define MAX_REPLAY_AGE 1800      // seconds; skip buffered frames older than 30 min on replay

// Deliver frames to each ready connector, advancing its cursor.
// `live`/`liveSeq` are the just-pushed live frame this tick (live==NULL if none):
// a connector caught up to it receives the REAL SondeInfo (full fidelity: extra/
// launchsite/rxStat), while a connector that is behind replays the cached copy
// (which carries only type/freq/afc/rssi/rxtime/d) until it catches up. idleTick()
// runs only when nothing was delivered, so SondeHub's batching/flush is preserved.
// Non-network connectors inherit replayReady()==false and are skipped.
void drainConnectors(SondeInfo *live, uint32_t liveSeq) {
  if (!frameCache.enabled()) return;
  uint32_t now = (uint32_t) time(NULL);
  // Value-initialize: frameCache.get() only fills type/freq/afc/rssi/rxtime/d, so
  // launchsite/rxStat/extra must start zeroed (empty launchsite, rxStat[0]=0,
  // extra=NULL) rather than leak stack garbage into replayed payloads.
  SondeInfo tmp = {};
  for (int i = 0; connectors[i]; i++) {
    Conn *c = connectors[i];
    if (c->replayCursor < frameCache.oldestSeq()) c->replayCursor = frameCache.oldestSeq();
    int delivered = 0;
    while (delivered < REPLAY_PACE && c->replayReady() && c->replayCursor < frameCache.headSeq()) {
      if (live && c->replayCursor == liveSeq) {
        c->updateSonde(live);         // caught up to the live frame: full fidelity, always fresh
      } else {
        if (!frameCache.get(c->replayCursor, &tmp)) { c->replayCursor++; continue; }
        // Age cap, guarded against a backward clock step (rxtime > now => treat as fresh).
        uint32_t age = (now >= tmp.rxtime) ? (now - tmp.rxtime) : 0;
        if (age > MAX_REPLAY_AGE) { c->replayCursor++; continue; }
        c->updateSonde(&tmp);
      }
      c->replayCursor++;
      delivered++;
    }
    if (delivered == 0) c->idleTick();
  }
}

void loopDecoder() {
  // Auto-scan mode: hold the peak-detected sonde while it keeps decoding; return
  // to sweeping only after norx_timeout seconds with no valid frame (same knob the
  // normal decoder uses for "no signal -> scan"). Tracked via the last good frame
  // so the decoder's own timeout/cycling can't trigger an early exit.
  if (autoscanActive() && sonde.config.norx_timeout > 0 &&
      (millis() - autoLockGoodMs) > (unsigned long)sonde.config.norx_timeout * 1000UL) {
    LOG_I(TAG, "AutoScan: no data for %ds, returning to scan\n", sonde.config.norx_timeout);
    enterMode(ST_AUTOSCAN);
    return;
  }
  // sonde knows the current type and frequency, and delegates to the right decoder
  uint16_t res = sonde.waitRXcomplete();
  // Refresh the auto-scan no-signal timer on a good frame from the locked sonde.
  if (autoscanActive() && (res & 0xff) == 0 && rxtask.receiveSonde == autoscanSlot())
    autoLockGoodMs = millis();
  int action;
  //LOG_D(TAG, "waitRX result is %x\n", (int)res);
  action = (int)(res >> 8);
  // TODO: update displayed sonde?

#if 0
  static int i = 0;
  if (i++ > 20) {
    i = 0;
    rtc_wdt_protect_off();
    rtc_wdt_disable();
    // Requires serial speed 921600, otherweise interrupt wdt will occur
    heap_caps_dump(MALLOC_CAP_8BIT);
  }
#endif

  if (action != ACT_NONE) {
    int newact = sonde.updateState(action);
    LOG_I(TAG, "loopDecoder: action %02x (%s) => %d  [current: main=%d, rxtask=%d]\n", action, action2text(action), newact, sonde.currentSonde, rxtask.currentSonde);
    action = newact;
    if (action != 255) {
      if (action == ACT_DISPLAY_SPECTRUM) {
        enterMode(ST_SPECTRUM);
        return;
      }
      else if (action == ACT_DISPLAY_WIFI) {
        enterMode(ST_WIFISCAN);
        return;
      }
#if FEATURE_RS92
      else if (action == ACT_RINEX_UPDATE) {
        enterMode(ST_RINEX_UPDATE);
        return;
      }
#endif
      else if (action == ACT_FORMAT_SD) {
        enterMode(ST_FORMAT_SD);
        return;
      }
    }
    // If action is to change to a different frequency, notify clients (MQTT and UDP-json for now)
    // TODO: do the same for TCP-json (rdzjson)
    if(ACT_IS_FREQ_CHANGE(action)) {
#if FEATURE_SONDESEEKER
      connSondeseeker.updateQRG(sonde.currentSonde);
#endif
#if FEATURE_MQTT
      connMQTT.updateQRG(sonde.currentSonde);
#endif
    }
  }

  if (rdzserver.hasClient()) {
    Serial.println("TCP JSON socket: new connection");
    rdzclient.stop();
    rdzclient = rdzserver.accept();
  }
  if (rdzclient.available()) {
    Serial.print("RDZ JSON socket: received ");
    while (rdzclient.available()) {
      char c = (char)rdzclient.read();
      Serial.print(c);
      if (c == '\n' || c == '}' || rdzDataPos >= RDZ_DATA_LEN - 1) {
        // parse GPS position from phone
        rdzData[rdzDataPos] = c;
        if (rdzDataPos > 2) parseGpsJson(rdzData, rdzDataPos + 1);
        rdzDataPos = 0;
      }
      else {
        rdzData[rdzDataPos++] = c;
      }
    }
    Serial.println("");
  }

  // wifi active and good packet received => send packet
  SondeInfo *s = &sonde.sondeList[rxtask.receiveSonde];
  bool goodFrame = ((res & 0xff) == 0);
  bool goodPos = s->d.validID && ((s->d.validPos & 0x03) == 0x03);
  if (goodFrame) s->rxtime = (uint32_t) time(NULL);   // receipt time; set even if !connected so buffered-during-outage frames replay with correct age/time_received

#if FEATURE_SERIALOUT
  if (goodFrame && goodPos) connSerialOut.updateSonde(s);
#endif

  if (frameCache.enabled()) {
    // Cache path: buffer frames worth uploading (valid id+position); network
    // connectors are fed via drainConnectors() (live frame full-fidelity when
    // caught up, cached copies for backfill). Local sinks are written live.
    SondeInfo *live = NULL;
    uint32_t liveSeq = 0;
    if (goodFrame && goodPos) {
      liveSeq = frameCache.push(s);   // buffered even if !connected, to cover WiFi outages too
      live = s;
    }
#if FEATURE_SDCARD
    if (goodFrame && connected) connSDCard.updateSonde(s);
#endif
#if FEATURE_NOTIFY
    // Live-only sink, like the SD card above: the alerts are about the sonde in front of you
    // now, so they must not queue behind a backfill.
    if (goodFrame && goodPos && connected) connNotify.updateSonde(s);
#endif
    drainConnectors(live, liveSeq);
  } else if ((res & 0xff) == 0 && connected) {
    // Direct dispatch, used when the cache is disabled.
    if (goodPos) {
#if FEATURE_APRS
      connAPRS.updateSonde(s);
#endif
#if FEATURE_CHASEMAPPER
      connChasemapper.updateSonde( s );
#endif
#if FEATURE_SONDESEEKER
      connSondeseeker.updateSonde( s );
#endif
#if FEATURE_NOTIFY
      connNotify.updateSonde( s );
#endif
    }
#if FEATURE_SONDEHUB
    connSondehub.updateSonde( s );   // invoke sh_send_data....
#endif
#if FEATURE_MQTT
    connMQTT.updateSonde( s );      // send to MQTT if enabled
#endif
#if FEATURE_SDCARD
    connSDCard.updateSonde(s);
#endif
  } else {
#if FEATURE_SONDEHUB
    connSondehub.updateSonde( NULL );
#endif
  }

  // Send own position periodically
#if FEATURE_MQTT
  connMQTT.updateStation( NULL );
#endif
#if FEATURE_APRS
  connAPRS.updateStation( NULL );
#endif
#if FEATURE_SONDEHUB
  connSondehub.updateStation( NULL );
#endif
#if FEATURE_SDCARD
  connSDCard.updateStation( NULL );
#endif
#if FEATURE_SERIALOUT
  connSerialOut.updateStation( NULL );
#endif
  // always send data, even if not valid....
  if (rdzclient.connected()) {
    Serial.println("Sending position via TCP as rdzJSON");
    char raw[1024];
    char gps[128];
    const char *typestr = s->d.typestr;
    if (*typestr == 0) typestr = sondeTypeStr[sonde.realType(s)];
    // TODO: only if GPS is valid...
    if (gpsPos.valid) {
      snprintf(gps, 128, ", \"gpslat\": %f,"
               "\"gpslon\": %f,"
               "\"gpsalt\": %d,"
               "\"gpsacc\": %d,"
               "\"gpsdir\": %d",
               gpsPos.lat, gpsPos.lon, gpsPos.alt, gpsPos.accuracy, gpsPos.course);
    } else {
      *gps = 0;
    }
    //
    raw[0] = '{';
    // Use same JSON format as for MQTT and HTML map........
    sonde2json(raw + 1, 1023, s);
    sprintf(raw + strlen(raw),
            ",\"active\":%d"
            ",\"validId\":%d"
            ",\"validPos\":%d"
            " %s}\n",
            (int)s->active,
            s->d.validID,
            s->d.validPos,
            gps);
    int len = strlen(raw);


    //Serial.println("Writing rdzclient...");
    if (len > 1024) len = 1024;
    int wlen = rdzclient.write(raw, len);
    if (wlen != len) {
      Serial.println("Writing rdzClient not OK, closing connection");
      rdzclient.stop();
    }
    //Serial.println("Writing rdzclient OK");
  }
  LOG_D(TAG, "updateDisplay started\n");
  sonde.dispsavectlOFF( (res & 0xff) == 0 );  // handle screen saver (disp auto off)
  if (forceReloadScreenConfig) {
    disp.initFromFile(sonde.config.screenfile);
    sonde.clearDisplay();
    forceReloadScreenConfig = false;
  }
  int t = millis();
  sonde.updateDisplay();
  LOG_D(TAG, "updateDisplay done (after %d ms)\n", (int)(millis() - t));
}

void setCurrentDisplay(int value) {
  LOG_D(TAG, "setCurrentDisplay: setting index %d, entry %d\n", value, sonde.config.display[value]);
  currentDisplay = sonde.config.display[value];
}

void loopSpectrum() {
  int marker = 0;
  char buf[10];
  uint8_t dispw, disph, dispxs, dispys;
  disp.rdis->getDispSize(&disph, &dispw, &dispxs, &dispys);

  switch (getKeyPress()) {
    case KP_SHORT: /* move selection of peak, TODO */
      sonde.nextConfig(); // TODO: Should be set specific frequency
      enterMode(ST_DECODER);
      return;
    case KP_MID: /* restart, TODO */ break;
    case KP_LONG:
      Serial.println("loopSpectrum: KP_LONG");
      enterMode(ST_WIFISCAN);
      return;
    case KP_DOUBLE:
      setCurrentDisplay(0);
      enterMode(ST_DECODER);
      return;
    default: break;
  }

  scanner.scan();
  scanner.plotResult();

  /*
    if(globalClient != NULL && globalClient->status() == WS_CONNECTED){
        String randomNumber = String(random(0,20));
        globalClient->text(randomNumber);
     }
  */

  if (sonde.config.spectrum > 0) {
    int remaining = sonde.config.spectrum - (millis() - specTimer) / 1000;
    LOG_D(TAG, "config.spectrum:%d  specTimer:%ld millis:%ld remaining:%d\n", sonde.config.spectrum, specTimer, millis(), remaining);
    if (sonde.config.marker != 0) {
      marker = 1;
    }
    snprintf(buf, 10, "%d Sec.", remaining);
    disp.rdis->drawString(0, dispys <= 1 ? (1 + marker) : (dispys + 1)*marker, buf);
    if (remaining <= 0) {
      setCurrentDisplay(0);
      enterMode(ST_DECODER);
    }
  }
}

void startSpectrumDisplay() {
  sonde.clearDisplay();
  disp.rdis->setFont(FONT_SMALL);
  disp.rdis->drawString(0, 0, "Spectrum Scan...");
  delay(500);
  enterMode(ST_SPECTRUM);
}

// ---------------- Auto-scan (peak detection) ----------------
// Active when autoscan_enable is set. Runs entirely in the main
// loop like loopSpectrum(): the RX background task is idle (mainState != ST_DECODER)
// so the main loop owns the radio. Each pass sweeps the spectrum, trial-decodes
// every peak through the enabled sonde types, and on a valid frame hands off to
// the normal decoder via enterMode(ST_DECODER, true). Modeled on radiosonde_auto_rx.
static const SondeType AUTOSCAN_TYPES[] = {
  STYPE_RS41, STYPE_DFM, STYPE_M10M20, STYPE_MP3H,
#if FEATURE_RS92
  STYPE_RS92,
#endif
};
#define AUTOSCAN_NTYPES ((int)(sizeof(AUTOSCAN_TYPES) / sizeof(AUTOSCAN_TYPES[0])))

// AS_QRG: try the active channel-list QRGs (configured freq+type) first, then the
// spectrum sweep + per-peak trial. AS_QRG is only entered when autoscan_qrgfirst is set.
enum { AS_QRG, AS_SWEEP, AS_TRIAL };
static int asState = AS_SWEEP;
static ScanPeak asPeaks[AUTOSCAN_MAXPK];
static int asNpeaks = 0;
static int asPeakIdx = 0;
static int asTypeIdx = 0;
static bool asTrialSetup = false;
static unsigned long asTrialStart = 0;
static unsigned long asNextSweep = 0;
static int asQrgIdx = 0;              // index into the channel list during AS_QRG
static bool asQrgSetup = false;       // radio tuned to the current QRG this cycle?
static unsigned long asQrgStart = 0;  // millis() the current QRG trial began

// QRG-first is active only when enabled AND there is at least one channel to try.
static inline bool autoscanQrgFirst() {
  return sonde.config.autoscan_qrgfirst != 0 && sonde.nSonde > 0;
}

// True if freqHz is within tolerance of any frequency (MHz) listed in the
// comma-separated autoscan_exclude config -- known local birdies/noise to skip
// during peak detection. Tolerance is half the peak-quantization step (so entering
// the frequency shown in the peak list matches), floored at 1 kHz.
static bool autoscanExcluded(double freqHz) {
  const char *s = sonde.config.autoscan_exclude;
  if (!s || !*s) return false;
  long tol = sonde.config.autoscan_quant > 0 ? sonde.config.autoscan_quant / 2 : 5000;
  if (tol < 1000) tol = 1000;
  while (*s) {
    while (*s == ' ' || *s == ',') s++;   // skip separators
    if (!*s) break;
    double fMHz = atof(s);
    if (fMHz > 0) {
      double d = freqHz - fMHz * 1e6;
      if (d < 0) d = -d;
      if (d <= (double)tol) return true;
    }
    while (*s && *s != ',') s++;           // advance to next token
  }
  return false;
}

static void autoscanReset() {
  asState = autoscanQrgFirst() ? AS_QRG : AS_SWEEP;
  asNpeaks = 0;
  asPeakIdx = asTypeIdx = 0;
  asTrialSetup = false;
  asNextSweep = 0;
  asQrgIdx = 0;
  asQrgSetup = false;
  autoscanWebState = 0;
  autoscanWebTryType = -1;
  autoscanWebNpeaks = 0;
  autoscanWebPeakN = 0;
  sonde.dispsavectlON();                             // start with the display on
}

void loopAutoScan() {
  // Auto-scan turned off at runtime (config change) -> resume normal decoding.
  if (!autoscanActive()) { setCurrentDisplay(1); enterMode(ST_DECODER); return; }
  // Buttons: let the user escape to the WiFi/config screen or manual spectrum.
  int key = getKeyPress();
  if (key != KP_NONE) sonde.dispsavectlON();         // any key wakes the display
  switch (key) {
    case KP_LONG: enterMode(ST_WIFISCAN); return;
    case KP_DOUBLE: enterMode(ST_SPECTRUM); return;
    default: break;
  }
  // Screen saver: scanning counts as "no RX", so the display dims/clears after the
  // configured timeout just like in decoder mode (called ~1x per pass, ~1-2 s).
  sonde.dispsavectlOFF(0);
  bool dispOn = (disp.dispstate != 0);

  if (asState == AS_QRG) {
    // QRG-first: before sweeping for peaks, walk the active channel-list entries and
    // try each on its own configured frequency+type. This catches known sondes whose
    // signal is too weak to stand out as a spectrum peak. One channel per loop pass.
    if (asNextSweep && (long)(millis() - asNextSweep) < 0) { delay(100); return; }  // brief pause between empty cycles
    while (asQrgIdx < sonde.nSonde && !sonde.sondeList[asQrgIdx].active) asQrgIdx++;  // skip inactive channels
    if (asQrgIdx >= sonde.nSonde) {                  // whole list tried, nothing locked -> sweep for peaks
      asState = AS_SWEEP;
      asNextSweep = 0;
      asQrgIdx = 0; asQrgSetup = false;
      autoscanWebState = 0;
      return;
    }

    // Give each QRG the same budget one sonde type gets in the peak trial.
    unsigned long perQrg = (sonde.config.autoscan_typedwell > 0)
        ? (unsigned long)sonde.config.autoscan_typedwell
        : (unsigned long)sonde.config.autoscan_dwell * 1000UL / AUTOSCAN_NTYPES;
    if (perQrg < 1000UL) perQrg = 1000UL;

    if (!asQrgSetup) {
      SondeInfo *ch = &sonde.sondeList[asQrgIdx];
      double fMHz = ch->freq;
      int slot = autoscanSlot();                     // reuse the scratch slot, like the peak trial,
      SondeInfo *si = &sonde.sondeList[slot];         // so the ST_DECODER handoff / autoLockGoodMs logic works
      si->type = ch->type;
      si->freq = fMHz;
      si->active = 1;
      rxtask.currentSonde = slot;
      sonde.currentSonde = slot;
      sonde.setup();
      asQrgStart = millis();
      asQrgSetup = true;
      autoscanWebState = 2;                           // 2 = QRG phase (0=sweep, 1=peak trial)
      autoscanWebTryFreq = fMHz;
      autoscanWebTryType = ch->type;
      LOG_I(TAG, "AutoScan: QRG %.3f MHz as %s\n", fMHz, sondeTypeStr[ch->type]);
      if (dispOn) {
        char buf[40];
        snprintf(buf, sizeof(buf), "QRG %.3f %s", fMHz, sondeTypeStr[ch->type]);
        disp.rdis->drawString(0, 0, buf);
      }
    }

    uint16_t res = sonde.rxRawFrame();               // ~1 frame window; blocks here
    if (res == RX_OK) {
      SondeInfo *si = &sonde.sondeList[autoscanSlot()];
      LOG_I(TAG, "AutoScan: LOCK (QRG) %.3f MHz as %s\n", si->freq, sondeTypeStr[si->type]);
      autoLockGoodMs = millis();                      // start the no-signal timer fresh
      sonde.dispsavectlON();
      setCurrentDisplay(1);
      enterMode(ST_DECODER, true);                    // hand off to the normal decoder
      return;
    }

    if ((long)(millis() - asQrgStart) >= (long)perQrg) {  // no frame in budget -> next active channel
      asQrgIdx++;
      asQrgSetup = false;
    }
    return;
  }

  if (asState == AS_SWEEP) {
    // Optional brief pause after a fruitless sweep so we don't spin uselessly.
    if (asNextSweep && (long)(millis() - asNextSweep) < 0) { delay(100); return; }
    scanner.scanForWeb();                            // sweep + bump web seq
    if (scanner.webSeq() && dispOn) scanner.plotResult();  // show spectrum (unless saver off)
    int maxpk = sonde.config.autoscan_maxpeaks;
    if (maxpk > AUTOSCAN_MAXPK) maxpk = AUTOSCAN_MAXPK;
    asNpeaks = scanner.findPeaks(asPeaks, maxpk, sonde.config.autoscan_snr,
                                 sonde.config.autoscan_mindist, sonde.config.autoscan_quant);
    // Drop peaks on configured known-noise frequencies (autoscan_exclude) so we don't
    // waste dwell trial-decoding a local birdie/spur. QRG channels are not filtered.
    {
      int kept = 0;
      for (int i = 0; i < asNpeaks; i++) {
        if (autoscanExcluded(asPeaks[i].freqHz)) {
          LOG_I(TAG, "AutoScan: excluding peak %.3f MHz (autoscan_exclude)\n", asPeaks[i].freqHz * 1e-6);
          continue;
        }
        if (kept != i) asPeaks[kept] = asPeaks[i];
        kept++;
      }
      asNpeaks = kept;
    }
    // Publish peak list to the web scan-plot.
    int pn = asNpeaks; if (pn > AUTOSCAN_MAXPK) pn = AUTOSCAN_MAXPK;
    for (int i = 0; i < pn; i++) autoscanWebPeakF[i] = asPeaks[i].freqHz * 1e-6;
    autoscanWebPeakN = pn;
    autoscanWebNpeaks = asNpeaks;
    LOG_I(TAG, "AutoScan: sweep found %d peaks\n", asNpeaks);
    if (asNpeaks == 0) {
      autoscanWebState = 0;
      if (autoscanQrgFirst()) {                        // re-check the QRG list next cycle
        asState = AS_QRG; asQrgIdx = 0; asQrgSetup = false;
      }
      asNextSweep = millis() + 1000UL;                 // brief delay before the next cycle
      return;
    }
    asPeakIdx = 0; asTypeIdx = 0; asTrialSetup = false;
    asState = AS_TRIAL;
    autoscanWebState = 1;
    return;
  }

  // AS_TRIAL: walk peaks (strongest first), trying each enabled type per peak.
  if (asPeakIdx >= asNpeaks) {
    if (autoscanQrgFirst()) {                         // start the next cycle with the QRG list
      asState = AS_QRG; asQrgIdx = 0; asQrgSetup = false;
      asNextSweep = 0;
    } else {
      asState = AS_SWEEP;
      asNextSweep = millis();                         // re-sweep immediately
    }
    autoscanWebState = 0;
    return;
  }

  // Per-type dwell: a fixed time per sonde type (autoscan_typedwell, ms) if set,
  // otherwise auto_rx's per-peak budget (autoscan_dwell, s) split across the
  // enabled types. Floored at ~one frame period so a present sonde can be caught.
  unsigned long perType = (sonde.config.autoscan_typedwell > 0)
      ? (unsigned long)sonde.config.autoscan_typedwell
      : (unsigned long)sonde.config.autoscan_dwell * 1000UL / AUTOSCAN_NTYPES;
  if (perType < 1000UL) perType = 1000UL;

  if (!asTrialSetup) {
    SondeType t = AUTOSCAN_TYPES[asTypeIdx];
    double fMHz = asPeaks[asPeakIdx].freqHz * 1e-6;
    int slot = autoscanSlot();                       // scratch slot past the channel list
    SondeInfo *si = &sonde.sondeList[slot];
    si->type = t;
    si->freq = fMHz;
    si->active = 1;
    rxtask.currentSonde = slot;
    sonde.currentSonde = slot;
    sonde.setup();
    asTrialStart = millis();
    asTrialSetup = true;
    autoscanWebTryFreq = fMHz;
    autoscanWebTryType = t;
    LOG_I(TAG, "AutoScan: trial %.3f MHz as %s\n", fMHz, sondeTypeStr[t]);
    if (dispOn) {
      char buf[40];
      snprintf(buf, sizeof(buf), "Scan %.3f %s", fMHz, sondeTypeStr[t]);
      disp.rdis->drawString(0, 0, buf);
    }
  }

  uint16_t res = sonde.rxRawFrame();                 // ~1 frame window; blocks here
  if (res == RX_OK) {
    SondeInfo *si = &sonde.sondeList[autoscanSlot()];
    LOG_I(TAG, "AutoScan: LOCK %.3f MHz as %s\n", si->freq, sondeTypeStr[si->type]);
    autoLockGoodMs = millis();                       // start the no-signal timer fresh
    sonde.dispsavectlON();                           // wake the display for the locked sonde
    setCurrentDisplay(1);                            // default sonde display
    enterMode(ST_DECODER, true);                     // hand off to the normal decoder
    return;
  }

  if ((long)(millis() - asTrialStart) >= (long)perType) {
    asTypeIdx++;
    asTrialSetup = false;
    if (asTypeIdx >= AUTOSCAN_NTYPES) {              // exhausted types -> next peak
      asTypeIdx = 0;
      asPeakIdx++;
    }
  }
}

const char *translateEncryptionType(wifi_auth_mode_t encryptionType) {
  switch (encryptionType) {
    case (WIFI_AUTH_OPEN):
      return "Open";
    case (WIFI_AUTH_WEP):
      return "WEP";
    case (WIFI_AUTH_WPA_PSK):
      return "WPA_PSK";
    case (WIFI_AUTH_WPA2_PSK):
      return "WPA2_PSK";
    case (WIFI_AUTH_WPA_WPA2_PSK):
      return "WPA_WPA2_PSK";
    case (WIFI_AUTH_WPA2_ENTERPRISE):
      return "WPA2_ENTERPRISE";
    default:
      return "";
  }
}

// in core.h
//enum t_wifi_state { WIFI_DISABLED, WIFI_SCAN, WIFI_CONNECT, WIFI_CONNECT_GOT_DISCONNECT, WIFI_CONNECTED, WIFI_APMODE };

volatile t_wifi_state wifi_state = WIFI_DISABLED;

uint32_t netup_time;

void enableNetwork(bool enable) {
  if (enable) {
    netup_time = esp_timer_get_time() / 1000000;
    MDNS.begin(sonde.config.mdnsname);
    SetupAsyncServer();
    udp.begin(WiFi.localIP(), LOCALUDPPORT);
    MDNS.addService("http", "tcp", 80);
    // => moved to conn-aprs  MDNS.addService("kiss-tnc", "tcp", 14580);
    MDNS.addService("jsonrdz", "tcp", 14570);
    //if (sonde.config.kisstnc.active) {
    //   tncserver.begin();
    rdzserver.begin();
    //}
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    connected = true;
#if FEATURE_MQTT
    connMQTT.netsetup();
#endif
#if FEATURE_SONDEHUB
    connSondehub.netsetup();
#endif
#if FEATURE_APRS
    connAPRS.netsetup();
#endif
  } else {
    MDNS.end();
#if FEATURE_MQTT
    connMQTT.netshutdown();
#endif
#if FEATURE_SONDEHUB
    connSondehub.netshutdown();
#endif
#if FEATURE_APRS
    connAPRS.netshutdown();
#endif
    connected = false;
  }

  Serial.println("enableNetwork done");
}


// Events used only for debug output right now
void WiFiEvent(WiFiEvent_t event)
{
  LOG_D(TAG, "[WiFi-event] event: %d\n", event);

  switch (event) {
    case ARDUINO_EVENT_WIFI_READY:
      Serial.println("WiFi interface ready");
      break;
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      Serial.println("Completed scan for access points");
      break;
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.println("WiFi client started");
      break;
    case ARDUINO_EVENT_WIFI_STA_STOP:
      Serial.println("WiFi clients stopped");
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("Connected to access point");
      if (wifi_state == WIFI_CONNECT_GOT_DISCONNECT) {
        /* Connection came back on its own; don't run the disconnect+retry path */
        wifi_state = WIFI_CONNECT;
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("Disconnected from WiFi access point");
      if (wifi_state == WIFI_CONNECT) {
        // If we get a disconnect event while waiting for connection (as I do sometimes with my FritzBox),
        // just start from scratch with WiFi scan
        //wifi_state = WIFI_DISABLED;
        //%WiFi.disconnect(true);
	// lets try somethign else:
	/// This is not a good idea, lets check what if we do nothing:   WiFi.reconnect();
	Serial.println("WiFi State was connect");
        wifi_state = WIFI_CONNECT_GOT_DISCONNECT;
	break;
      }
      LOG_D(TAG, "Turning off (state is %d)\n", wifi_state);
      // Only actually power the radio down when WiFi is configured off (mode 0).
      // For every other mode we want to stay reachable -- either reconnect as a
      // station or keep the AP up (incl. the mode-5 AP+STA fallback) -- so leave
      // the radio on and let loopWifiBackground() drive recovery. Powering off
      // here would kill the radio whenever an established station link drops,
      // leaving no way to reconnect.
      if (sonde.config.wifi != 0) break;
      WiFi.mode(WIFI_MODE_NULL);
      break;
    case ARDUINO_EVENT_WIFI_OFF:
      Serial.println("WiFi is OFF");
      /* So let's retry? depending on mode... for now testing for mode 4*/
      if(sonde.config.wifi == 4) { wifiConnectDirect(1); }
      break;
    case ARDUINO_EVENT_WIFI_STA_AUTHMODE_CHANGE:
      Serial.println("Authentication mode of access point has changed");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("Obtained IP address: ");
      Serial.println(WiFi.localIP());
      WiFi.STA.dnsIP(2, 0x08080808);
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.println("Lost IP address and IP address is reset to 0");
      break;
    case ARDUINO_EVENT_WPS_ER_SUCCESS:
      Serial.println("WiFi Protected Setup (WPS): succeeded in enrollee mode");
      break;
    case ARDUINO_EVENT_WPS_ER_FAILED:
      Serial.println("WiFi Protected Setup (WPS): failed in enrollee mode");
      break;
    case ARDUINO_EVENT_WPS_ER_TIMEOUT:
      Serial.println("WiFi Protected Setup (WPS): timeout in enrollee mode");
      break;
    case ARDUINO_EVENT_WPS_ER_PIN:
      Serial.println("WiFi Protected Setup (WPS): pin code in enrollee mode");
      break;
    case ARDUINO_EVENT_WIFI_AP_START:
      Serial.println("WiFi access point started");
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP:
      Serial.println("WiFi access point  stopped");
      break;
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      Serial.println("Client connected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      Serial.println("Client disconnected");
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      Serial.println("Assigned IP address to client");
      break;
    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
      Serial.println("Received probe request");
      break;
    case ARDUINO_EVENT_WIFI_AP_GOT_IP6:
      Serial.println("AP IPv6 is preferred");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP6:
      Serial.println("STA obtained IPv6");
      Serial.println(WiFi.STA.globalIPv6());
      Serial.println(WiFi.STA.linkLocalIPv6());
      break;
    case ARDUINO_EVENT_ETH_GOT_IP6:
      Serial.println("Ethernet IPv6 is preferred");
      break;
    case ARDUINO_EVENT_ETH_START:
      Serial.println("Ethernet started");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("Ethernet stopped");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("Ethernet connected");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("Ethernet disconnected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("Obtained IP address");
      break;
    default:
      break;
  }
}


// Time budget for a single station (re)connect attempt. This is a wall-clock
// deadline rather than a loop-iteration count: loopWifiBackground() is paced by
// the RX loop (waitRXcomplete), whose cadence varies with sonde type/signal, so
// a fixed iteration count would give an unpredictable timeout. Armed at every WiFi.begin
// (wifiConnect/wifiConnectDirect/loopWifiScan) so the background loop always has a
// valid deadline for a connect attempt handed off to it.
#define WIFI_CONNECT_TIMEOUT_MS 20000UL
static unsigned long wifi_connect_deadline;

void wifiConnect(int16_t res) {
  LOG_I(TAG, "WiFi scan result: found %d networks\n", res);

  // pick best network
  int bestEntry = -1;
  int bestRSSI = INT_MIN;
  uint8_t bestBSSID[6];
  int32_t bestChannel = 0;

  for (int8_t i = 0; i < res; i++) {
    String ssid_scan;
    int32_t rssi_scan;
    uint8_t sec_scan;
    uint8_t* BSSID_scan;
    int32_t chan_scan;
    WiFi.getNetworkInfo(i, ssid_scan, sec_scan, rssi_scan, BSSID_scan, chan_scan);
    int networkEntry = fetchWifiIndex(ssid_scan.c_str());
    if (networkEntry < 0) continue;
    if (rssi_scan <= bestRSSI) continue;
    bestEntry = networkEntry;
    bestRSSI = rssi_scan;
    bestChannel = chan_scan;
    memcpy((void*) &bestBSSID, (void*) BSSID_scan, sizeof(bestBSSID));
  }
  WiFi.scanDelete();
  if (bestEntry >= 0) {
    LOG_D(TAG, "WiFi Connecting BSSID: %02X:%02X:%02X:%02X:%02X:%02X SSID: %s PW %s Channel: %d (RSSI %d)\n",
      bestBSSID[0], bestBSSID[1], bestBSSID[2], bestBSSID[3], bestBSSID[4], bestBSSID[5],
      fetchWifiSSID(bestEntry), fetchWifiPw(bestEntry), bestChannel, bestRSSI);
    wifi_state = WIFI_CONNECT;
    WiFi.begin(fetchWifiSSID(bestEntry), fetchWifiPw(bestEntry), bestChannel, bestBSSID);
    wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
  } else {
    // rescan
    // wifiStart();
    WiFi.disconnect(true);
    wifi_state = WIFI_DISABLED;
  }
}

void wifiConnectDirect(int16_t index) {
  // Mode 4 uses networks[1] (index 0 is the fallback AP's identity). If that slot
  // isn't configured, WiFi.begin("","") would silently connect to nothing; bail
  // with a clear log instead so the misconfiguration is visible.
  if (index < 0 || index >= nNetworks || strlen(fetchWifiSSID(index)) == 0) {
    LOG_E(TAG, "WiFi mode 4 (direct): network slot %d not configured (nNetworks=%d) -- check networks.txt\n", index, nNetworks);
    return;
  }
  Serial.println("AP mode 4: trying direct reconnect");
  wifi_state = WIFI_CONNECT;
  WiFi.begin(fetchWifiSSID(index), fetchWifiPw(index));
  wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
}

// Mode 5: after this many failed background scan/connect cycles, give up on a
// pure-station reconnect and re-raise the AP (AP+STA) so the device stays
// reachable while it keeps retrying the configured network in the background.
#define WIFI_MODE5_AP_FALLBACK 3
static int wifi_reconnect_fails;

// Mode 5 (config.wifi==5) AP fallback: keep the AP up but retry the configured
// station network in the background (AP+STA). On success the AP is dropped.
#define AP_STA_RETRY_MS   120000UL   // gap between background station retry attempts
#define AP_STA_CONNECT_MS  15000UL   // time allowed for each background connect attempt
static unsigned long apsta_next_retry = 0;
static unsigned long apsta_connect_deadline = 0;
static int apsta_phase = 0;          // 0=waiting, 1=scanning, 2=connecting

void loopWifiBackground() {
  LOG_D(TAG, "WifiBackground: state %d\n", wifi_state);
  // handle Wifi station mode in background
  if (sonde.config.wifi == 0 || sonde.config.wifi == 2) return; // nothing to do if disabled or access point mode

  if (wifi_state == WIFI_DISABLED) {  // stopped => start scan/connect
    if (sonde.config.wifi == 4) {  // direct connect to first network, supports hidden SSID
       wifiConnectDirect(1);       // arms wifi_connect_deadline
    } else if (sonde.config.wifi == 5 && wifi_reconnect_fails >= WIFI_MODE5_AP_FALLBACK) {
      // Mode 5: could not restore the station link after several cycles. Bring the
      // AP back (AP+STA) so the device stays reachable while the apsta background
      // logic keeps retrying the configured network. (At boot loopWifiScan() does
      // this fallback; without it a runtime loss loops as pure STA forever and the
      // device becomes unreachable when the configured network is gone for good.)
      Serial.println("WiFi mode 5: reconnect failed repeatedly -- re-raising AP");
      wifi_reconnect_fails = 0;
      startAP();
      enableNetwork(true);
    } else {
      Serial.println("WiFi start scan");
      if (sonde.config.wifi == 5) wifi_reconnect_fails++;
      wifi_state = WIFI_SCAN;
      WiFi.scanNetworks(true); // scan in async mode
    }
  } else if (wifi_state == WIFI_SCAN) {
    int16_t res = WiFi.scanComplete();
    if (res == 0 || res == WIFI_SCAN_FAILED) {
      // retry
      Serial.println("WiFi restart scan");
      WiFi.disconnect(true);
      wifi_state = WIFI_DISABLED;
      return;
    }
    if (res == WIFI_SCAN_RUNNING) {
      return;
    }
    // Scan finished, try to connect
    wifiConnect(res);             // arms wifi_connect_deadline on success
  } else if (wifi_state == WIFI_CONNECT) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Wifi is connected\n");
      wifi_state = WIFI_CONNECTED;
      wifi_reconnect_fails = 0;   // reconnect succeeded; reset the AP-fallback counter
      // update IP in display
      String localIPstr = WiFi.localIP().toString();
      LOG_I(TAG, "IP is %s\n", localIPstr.c_str());
      sonde.setIP(localIPstr.c_str(), false);
      sonde.updateDisplayIP();
      enableNetwork(true);
    }
    else if ((long)(millis() - wifi_connect_deadline) >= 0) { // timed out, restart scanning
      wifi_state = WIFI_DISABLED;
      WiFi.disconnect(true);
    }
  } else if (wifi_state == WIFI_CONNECT_GOT_DISCONNECT) {
    // A disconnect event arrived while a background connect attempt was in progress
    // (loopWifiScan() retries this inline; the background loop needs its own case,
    // otherwise a dropped/failed reconnect stays parked here and never recovers).
    // Either the link came back on its own, or we fall back to a fresh scan/connect cycle.
    if (WiFi.status() == WL_CONNECTED) {
      wifi_state = WIFI_CONNECT;   // came back; let WIFI_CONNECT finish the handshake
    } else {
      WiFi.disconnect(true);
      wifi_state = WIFI_DISABLED;  // restart scan -> connect
    }
  } else if (wifi_state == WIFI_CONNECTED) {
    //LOG_D(TAG, "status: %d\n", ((WiFiSTAClass)WiFi).status());
    if (WiFi.status() != WL_CONNECTED) {
      sonde.setIP("", false);
      sonde.updateDisplayIP();

      wifi_state = WIFI_DISABLED;  // restart scan
      enableNetwork(false);
      WiFi.disconnect(true);
    } //else Serial.println("WiFi still connected");
  } else if (wifi_state == WIFI_APMODE) {
    // Mode 5 AP fallback: periodically retry the configured station network in the
    // background (AP+STA) without dropping the AP; once connected, drop the AP.
    if (sonde.config.wifi != 5) return;   // only mode 5 retries; other AP modes stay put
    unsigned long now = millis();
    if (apsta_phase == 0) {                       // waiting for the next retry window
      if ((long)(now - apsta_next_retry) < 0) return;
      Serial.println("AP+STA: background station retry -- scanning");
      WiFi.scanNetworks(true);                    // async scan, AP stays up
      apsta_phase = 1;
    } else if (apsta_phase == 1) {                // scan in progress
      int16_t res = WiFi.scanComplete();
      if (res == WIFI_SCAN_RUNNING) return;
      apsta_phase = 0;
      apsta_next_retry = now + AP_STA_RETRY_MS;    // schedule next window regardless
      if (res <= 0) { WiFi.scanDelete(); return; } // failed/empty scan, try again later
      // pick the strongest configured network that is in range
      int bestEntry = -1; int bestRSSI = INT_MIN; int32_t bestChannel = 0;
      uint8_t bestBSSID[6];
      for (int i = 0; i < res; i++) {
        String ssid; int32_t rssi; uint8_t sec; uint8_t *bssid; int32_t chan;
        WiFi.getNetworkInfo(i, ssid, sec, rssi, bssid, chan);
        int idx = fetchWifiIndex(ssid.c_str());
        if (idx < 0 || rssi <= bestRSSI) continue;
        bestEntry = idx; bestRSSI = rssi; bestChannel = chan;
        memcpy(bestBSSID, bssid, sizeof(bestBSSID));
      }
      WiFi.scanDelete();
      if (bestEntry < 0) return;                   // configured network not in range
      LOG_I(TAG, "AP+STA: connecting to %s in background\n", fetchWifiSSID(bestEntry));
      WiFi.begin(fetchWifiSSID(bestEntry), fetchWifiPw(bestEntry), bestChannel, bestBSSID);
      apsta_phase = 2;
      apsta_connect_deadline = now + AP_STA_CONNECT_MS;
    } else if (apsta_phase == 2) {                // connect attempt in progress
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("AP+STA: station connected -- dropping AP");
        WiFi.softAPdisconnect(true);              // drop the AP, keep the station link
        WiFi.mode(WIFI_STA);
        String localIPstr = WiFi.localIP().toString();
        LOG_I(TAG, "IP is %s\n", localIPstr.c_str());
        sonde.setIP(localIPstr.c_str(), false);
        sonde.updateDisplayIP();
        wifi_state = WIFI_CONNECTED;
        enableNetwork(true);                       // rebind services to the station IP
        apsta_phase = 0;
      } else if ((long)(now - apsta_connect_deadline) >= 0) {
        Serial.println("AP+STA: station retry timed out -- staying in AP mode");
        WiFi.disconnect(false);                    // abort the STA attempt, keep the AP
        apsta_phase = 0;
        apsta_next_retry = now + AP_STA_RETRY_MS;
      }
    }
  }
}

void startAP() {
  Serial.println("Activating access point mode");
  wifi_state = WIFI_APMODE;
  // Mode 5 keeps the station radio enabled (AP+STA) so it can retry the configured
  // network in the background without dropping the AP; other modes run the AP alone.
  if (sonde.config.wifi == 5) {
    WiFi.mode(WIFI_AP_STA);
    apsta_phase = 0;
    apsta_next_retry = millis() + AP_STA_RETRY_MS;
  } else {
    WiFi.mode(WIFI_AP);
  }
  WiFi.softAP(networks[0].id.c_str(), networks[0].pw.c_str());

  Serial.println("Wait 100 ms for AP_START...");
  delay(100);
  Serial.println(WiFi.softAPConfig(IPAddress (192, 168, 4, 1), IPAddress (0, 0, 0, 0), IPAddress (255, 255, 255, 0)) ? "Ready" : "Failed!");

  IPAddress myIP = WiFi.softAPIP();
  String myIPstr = myIP.toString();
  sonde.setIP(myIPstr.c_str(), true);
  sonde.updateDisplayIP();
  // enableNetwork(true); done later in WifiLoop.
}

void initialMode() {
  if (sonde.config.touch_thresh == 0) {
    enterMode(ST_TOUCHCALIB);
    return;
  }
  if (sonde.config.spectrum != -1) {    // enable Spectrum in config.txt: spectrum=number_of_seconds
    startSpectrumDisplay();
  } else {
    setCurrentDisplay(0);
    enterMode(ST_DECODER);
  }
}

void loopTouchCalib() {
  uint8_t dispw, disph, dispxs, dispys;
  disp.rdis->clear();
  disp.rdis->getDispSize(&disph, &dispw, &dispxs, &dispys);
  char num[10];

  while (1) {
    int t1 = touchRead(button1.pin & 0x7f);
    int t2 = touchRead(button2.pin & 0x7f);
    disp.rdis->setFont(FONT_LARGE);
    disp.rdis->drawString(0, 0, "Touch calib.");
    disp.rdis->drawString(0, 3 * dispys, "Touch1: ");
    snprintf(num, 10, "%d  ", t1);
    disp.rdis->drawString(8 * dispxs, 3 * dispys, num);
    disp.rdis->drawString(0, 6 * dispys, "Touch2: ");
    snprintf(num, 10, "%d  ", t2);
    disp.rdis->drawString(8 * dispxs, 6 * dispys, num);
    delay(300);
  }
}

// Wifi modes
// 0: disabled. directly start initial mode (spectrum or scanner)
// 1: Station mode, new version: start with synchronous WiFi scan, then
//    - if button was pressed, switch to AP mode
//    - if connect successful, all good
//    - otherwise, continue with station mode in background
// 2: access point mode (wait for clients in background)
// 3: traditional sync. WifiScan. Tries to connect to a network, in case of failure activates AP.
// 4: Station mode/hidden AP: same as 1, but instead of scan, just call espressif method to connect (will connect to hidden AP as well
// 5: like 3, but keeps the AP up and retries the station connection in the background (AP+STA); drops the AP once the station connects
#define MAXWIFIDELAY 40
static const char* _scan[2] = {"/", "\\"};
void loopWifiScan() {
  getKeyPressEvent(); // Clear any old events
  enableNetwork(false);
  WiFi.disconnect(true);
  wifi_state = WIFI_DISABLED;
  disp.rdis->setFont(FONT_SMALL);
  uint8_t dispw, disph, dispxs, dispys;
  disp.rdis->getDispSize(&disph, &dispw, &dispys, &dispxs);
  int dwidth = dispw / dispxs;
  int lastl = (disph / dispys - 2) * dispys;
  int cnt = 0;
  char abort = 0; // abort on keypress
  int net_index = -1;

  switch(sonde.config.wifi) {
  case 0:  // no WiFi
    initialMode();
    return;
  case 2:  // AP mode, setup in background
    startAP();
    enableNetwork(true);
    initialMode();
    return;
  case 4:  // direct connect without scan, only first item in network list
    // Mode STN/DIRECT[4]: Connect directly (supports hidden AP)
    {
      if (nNetworks < 2 || strlen(fetchWifiSSID(1)) == 0) {
        // No station network configured for direct connect: fall back to AP so
        // the user can reach the web UI and fix networks.txt.
        LOG_E(TAG, "WiFi mode 4 (direct): no station network configured -- falling back to AP\n");
        abort = 1;
      } else {
        disp.rdis->drawString(0, 0, "WiFi Connect...");
        disp.rdis->drawString(0, dispys * 2, fetchWifiSSID(1));
        wifiConnectDirect(1);
      }
    }
    break;
  case 1:  // STATION mode (continue in BG if no connection)
  case 3:  // old AUTO mode (change to AP if no connection)
  case 5:  // like AUTO, but the AP stays up and the station is retried in background (AP+STA)
    // Mode STATION[1] or SETUP[3] or AP+retry[5]: Scan for networks;
    disp.rdis->drawString(0, 0, "WiFi Scan...");
    int line = 0;
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      disp.rdis->drawString(0, dispys * (1 + line), ssid.c_str(), dwidth);
      line = (line + 1) % (disph / dispys);
      String mac = WiFi.BSSIDstr(i);
      const char *encryptionTypeDescription = translateEncryptionType(WiFi.encryptionType(i));
      LOG_I(TAG, "Network %s: RSSI %d, MAC %s, enc: %s\n", ssid.c_str(), WiFi.RSSI(i), mac.c_str(), encryptionTypeDescription);
      int curidx = fetchWifiIndex(ssid.c_str());
      if (curidx >= 0 && net_index == -1) {
        net_index = curidx;
        LOG_I(TAG, "Match found at scan entry %d, config network %d\n", i, net_index);
      }
    }
    if (net_index >= 0) { // some network was found
      Serial.print("Connecting to: "); Serial.print(fetchWifiSSID(net_index));
      Serial.print(" with password "); Serial.println(fetchWifiPw(net_index));

      disp.rdis->drawString(0, lastl, "Conn:");
      disp.rdis->drawString(6 * dispxs, lastl, fetchWifiSSID(net_index));
      // TODO: wifi_state is used inconsistently
      wifi_state = WIFI_CONNECT;
      WiFi.begin(fetchWifiSSID(net_index), fetchWifiPw(net_index));
      // Arm the timeout in case this attempt is still pending when loopWifiScan()
      // hands off to loopWifiBackground() (mode 1 continues connecting in the BG).
      wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;
    } else {
      abort = 2;  // no network found in scan => abort right away
    }
  }
  while (WiFi.status() != WL_CONNECTED && cnt < MAXWIFIDELAY && !abort)  {
    delay(500);
    if(wifi_state == WIFI_CONNECT_GOT_DISCONNECT) {
      if(WiFi.status() == WL_CONNECTED) { /* connection came back, avoid tearing it down */
        wifi_state = WIFI_CONNECT;
      } else {
        int connectIndex = (sonde.config.wifi == 4) ? 1 : net_index;
        WiFi.disconnect(true);   // Disconnect and wait, allowing full dissassociation, then retry
        Serial.print("_d_");
        delay(2000); // 2sec delay
        handlePMUirq();
        abort = (getKeyPressEvent() != EVT_NONE);
        if(abort) break;
        WiFi.begin(fetchWifiSSID(connectIndex), fetchWifiPw(connectIndex));
        wifi_state = WIFI_CONNECT;
        wifi_connect_deadline = millis() + WIFI_CONNECT_TIMEOUT_MS;  // arm for a possible BG handoff
      }
    }
    Serial.print(".");
    disp.rdis->drawString(15 * dispxs, lastl + dispys, _scan[cnt & 1]);
    cnt++;
    handlePMUirq();    // Needed to react to PMU chip button
    abort = (getKeyPressEvent() != EVT_NONE);
  }
  // We reach this point for mode 1, 3, and 4
  // If connected (in any case) => all good, download eph if needed, all up and running
  // Otherwise, If key was pressed, switch to AP mode
  // Otherwise, if mode is 3 (old AUTO), switch to AP mode
  // Otherwise, no network yet, keep trying to activate network in background (loopWiFiBackground)
  if(WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected\nIP address:");
    String localIPstr = WiFi.localIP().toString();
    Serial.println(localIPstr);
    sonde.setIP(localIPstr.c_str(), false);
    sonde.updateDisplayIP();
    wifi_state = WIFI_CONNECTED;
#if FEATURE_RS92
    bool hasRS92 = false;
    for (int i = 0; i < MAXSONDE; i++) {
      if (sonde.sondeList[i].type == STYPE_RS92) hasRS92 = true;
    }
    if (hasRS92) {
      geteph();
      if (ephstate == EPH_PENDING) ephstate = EPH_ERROR;
      get_eph("/brdc");
    }
#endif
    enableNetwork(true);
    delay(3000);
  }
  else if(sonde.config.wifi == 3 || sonde.config.wifi == 5 || abort==1 ) {
    WiFi.disconnect(true);
    delay(1000);
    startAP();
    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(myIP);
    disp.rdis->drawString(0, lastl, "AP:             ");
    disp.rdis->drawString(6 * dispxs, lastl + 1, networks[0].id.c_str());
    enableNetwork(true);
    delay(3000);
  }
  initialMode();
}

#if FEATURE_RS92
// Rinex update...
void execRinexUpdate() {
   Serial.println("Fetching update for RINEX data...\n");
   geteph();
   Serial.println("Reading RINEX data...\n");
   if (ephstate == EPH_PENDING) ephstate = EPH_ERROR;
   get_eph("/brdc");
   setCurrentDisplay(0);
   enterMode(ST_DECODER);
}
#endif

#if FEATURE_SDCARD
void execFormatSD() {
   Serial.println("format SD card\n");
   if ( ISOLED(sonde.config) ) {
       disp.rdis->setFont(FONT_SMALL);
   } else {
       disp.rdis->setFont(5);
   }
   disp.rdis->drawString(0, 0, "Format SD card");

   connSDCard.format();
   setCurrentDisplay(0);
   enterMode(ST_DECODER);
}
#endif

/// Testing OTA Updates
/// somewhat based on Arduino's AWS_S3_OTA_Update
// Utility to extract header value from headers
String getHeaderValue(String header, String headerName) {
  return header.substring(strlen(headerName.c_str()));
}


// OTA Logic
void execOTA() {
  int contentLength = 0;
  bool isValidContentType = false;
  sonde.clearDisplay();
  uint8_t dispxs, dispys;
  Serial.printf("Updater connecting to: %s:%d%s\n", updateHost, updatePort, updatePrefix);

  if ( ISOLED(sonde.config) ) {
    disp.rdis->setFont(FONT_SMALL);
    dispxs = dispys = 1;
    char uh[17];
    strncpy(uh, updateHost, 17);
    uh[16] = 0;
    disp.rdis->drawString(0, 0, uh);
  } else {
    disp.rdis->setFont(5);
    dispxs = 18;
    dispys = 20;
    disp.rdis->drawString(0, 0, updateHost);
  }

  // Connect to Update host
  if (!client.connect(updateHost, updatePort)) {
    LOG_E(TAG, "Connection to %s:%d for fs update failed\n", updateHost, updatePort);
    enterMode(ST_DECODER);
    return;
  }

  // First, try update file system
  LOG_I(TAG, "Fetching fs update from '%s:%d' '%s' '%s'\n", updateHost, updatePort, updatePrefix, updateFs);
  disp.rdis->drawString(0, 1 * dispys, "Fetching fs...");
  client.printf("GET %s%s HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n\r\n", updatePrefix, updateFs, updateHost, updatePort);
  // see if we get some data....

  int type = 0;
  int res = fetchHTTPheader(&type);
  if (res < 0) {
    ; // no-op
  } else {
    // Unpack the filesystem archive directly from the network stream (shared with the
    // web file-upload path, see unpackFsArchive).
    disp.rdis->drawString(0, 2 * dispys, "Updating files");
    if (unpackFsArchive(client) < 0) {
      client.stop();
      enterMode(ST_DECODER);
      return;
    }
    client.stop();
  }

  // Connect to Update host
  if (!client.connect(updateHost, updatePort)) {
    LOG_E(TAG, "Connection to %s:%d for app update failed\n", updateHost, updatePort);
    enterMode(ST_DECODER);
    return;
  }

  // Connection succeeded, fecthing the bin
  LOG_I(TAG, "Fetching bin update from '%s:%d' '%s' '%s'", updateHost, updatePort, updatePrefix, updateIno);
  disp.rdis->drawString(0, 3 * dispys, "Fetching update");

  // Get the contents of the bin file
  client.printf("GET %s%s HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n\r\n",
                updatePrefix, updateIno, updateHost, updatePort);

  // Check what is being sent
  //    Serial.print(String("GET ") + bin + " HTTP/1.1\r\n" +
  //                 "Host: " + host + "\r\n" +
  //                 "Cache-Control: no-cache\r\n" +
  //                 "Connection: close\r\n\r\n");

  int validType = 0;
  contentLength = fetchHTTPheader( &validType );
  if (validType == 1) isValidContentType = true;

  // Check what is the contentLength and if content type is `application/octet-stream`
  Serial.printf("contentLength : %d, isValidContentType : %d\n",contentLength, isValidContentType);
  disp.rdis->drawString(0, 4 * dispys, "Len: ");
  String cls = String(contentLength);
  disp.rdis->drawString(5 * dispxs, 4 * dispys, cls.c_str());

  // check contentLength and content type
  if (contentLength && isValidContentType) {
    // Check if there is enough to OTA Update
    bool canBegin = Update.begin(contentLength);

    // If yes, begin
    if (canBegin) {
      disp.rdis->drawString(0, 5 * dispys, "Starting update");
      Serial.println("Begin OTA. This may take 2 - 5 mins to complete. Things might be quite for a while.. Patience!");
      // No activity would appear on the Serial monitor
      // So be patient. This may take 2 - 5mins to complete
      size_t written = Update.writeStream(client);

      if (written == contentLength) {
        Serial.println("Written : " + String(written) + " successfully");
      } else {
        Serial.println("Written only : " + String(written) + "/" + String(contentLength) + ". Retry?" );
        // retry??
        // execOTA();
      }

      if (Update.end()) {
        Serial.println("OTA done!");
        if (Update.isFinished()) {
          Serial.println("Update successfully completed. Rebooting.");
          disp.rdis->drawString(0, 7 * dispys, "Rebooting....");
          delay(1000);
          ESP.restart();
        } else {
          Serial.println("Update not finished? Something went wrong!");
        }
      } else {
        Serial.println("Error Occurred. Error #: " + String(Update.getError()));
      }
    } else {
      // not enough space to begin OTA
      // Understand the partitions and
      // space availability
      Serial.println("Not enough space to begin OTA");
      client.clear();
    }
  } else {
    Serial.println("There was no content in the response");
    client.clear();
  }
  // Back to some normal state
  enterMode(ST_DECODER);
}

int fetchHTTPheader(int *validType) {
  int contentLength = -1;
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 5000) {
      Serial.println("Client Timeout !");
      client.stop();
      return -1;
    }
  }
  // Once the response is available, check stuff

  /*
     Response Structure
      HTTP/1.1 200 OK
      x-amz-id-2: NVKxnU1aIQMmpGKhSwpCBh8y2JPbak18QLIfE+OiUDOos+7UftZKjtCFqrwsGOZRN5Zee0jpTd0=
      x-amz-request-id: 2D56B47560B764EC
      Date: Wed, 14 Jun 2017 03:33:59 GMT
      Last-Modified: Fri, 02 Jun 2017 14:50:11 GMT
      ETag: "d2afebbaaebc38cd669ce36727152af9"
      Accept-Ranges: bytes
      Content-Type: application/octet-stream
      Content-Length: 357280
      Server: AmazonS3

      {{BIN FILE CONTENTS}}

  */
  while (client.available()) {
    // read line till \n
    String line = client.readStringUntil('\n');
    // remove space, to check if the line is end of headers
    line.trim();

    // if the the line is empty,
    // this is end of headers
    // break the while and feed the
    // remaining `client` to the
    // Update.writeStream();
    if (!line.length()) {
      //headers ended
      break; // and get the OTA started
    }

    // Check if the HTTP Response is 200
    // else break and Exit Update
    if (line.startsWith("HTTP/1.1")) {
      if (line.indexOf("200") < 0) {
        Serial.println("Got a non 200 status code from server. Exiting OTA Update.");
        return -1;
      }
    }

    // extract headers here
    // Start with content length
    static const char *HEADER_CL = "Content-Length: ";
    if (strncasecmp( line.c_str(), HEADER_CL, strlen(HEADER_CL) ) == 0 ) {
      contentLength = atoi( line.c_str() + strlen(HEADER_CL) );
      Serial.println("Got " + String(contentLength) + " bytes from server");
    }

    // Next, the content type
    static const char *HEADER_CT = "Content-Type: ";
    if (strncasecmp( line.c_str(), HEADER_CT, strlen(HEADER_CT) ) == 0 ) {
      const char *contentType = line.c_str() + strlen(HEADER_CT);
      LOG_I(TAG, "Content type: %s\n", contentType);
      if (strcmp(contentType, "application/octet-stream")==0) {
        if (validType) *validType = 1;
      }
    }
  }
  return contentLength;
}



void loop() {
  LOG_I(TAG, "Running loop in state %d [currentDisp:%d, lastDisp:%d]. free heap: %d, unused stack: %d\n",
                mainState, currentDisplay, lastDisplay, ESP.getFreeHeap(), uxTaskGetStackHighWaterMark(0));

  Log.handleImprov();

  // Deferred reboot requested by the web file-upload OTA (after its response was sent).
  if (otaRebootAt && millis() > otaRebootAt) { Serial.println("Rebooting after file upload"); ESP.restart(); }

#ifndef REMOVE_ALL_FOR_TESTING
  switch (mainState) {
    case ST_DECODER:
#ifndef DISABLE_MAINRX
      loopDecoder();
#else
      delay(1000);
#endif
      break;
    case ST_SPECTRUM: loopSpectrum(); break;
    case ST_AUTOSCAN: loopAutoScan(); break;
    case ST_WIFISCAN: loopWifiScan(); break;
    case ST_UPDATE: execOTA(); break;
    case ST_TOUCHCALIB: loopTouchCalib(); break;
#if FEATURE_RS92
    case ST_RINEX_UPDATE: execRinexUpdate(); break;
#endif
#if FEATURE_SDCARD
    case ST_FORMAT_SD: execFormatSD(); break;
#endif
  }
#if 0
  int rssi = sx1278.getRSSI();
  Serial.print("  RSSI: ");
  Serial.print(rssi);

  int gain = sx1278.getLNAGain();
  Serial.print(" LNA Gain: "),
               Serial.println(gain);
#endif
  loopWifiBackground();
  if (currentDisplay != lastDisplay && (mainState == ST_DECODER)) {
    disp.setLayout(currentDisplay);
    sonde.clearDisplay();
    sonde.updateDisplay();
    lastDisplay = currentDisplay;
  }
#else
  delay(1000);
#endif
}


