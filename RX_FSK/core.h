enum t_wifi_state { WIFI_DISABLED, WIFI_SCAN, WIFI_CONNECT, WIFI_CONNECT_GOT_DISCONNECT, WIFI_CONNECTED, WIFI_APMODE };
// Written from both the WiFi system-event task (WiFiEvent) and the main loop
// (loopWifiBackground/loopWifiScan), so it must be volatile to avoid a stale
// cached value hiding a state transition made by the other context.
extern volatile t_wifi_state wifi_state;

extern const char *sondeTypeStrSH[];
