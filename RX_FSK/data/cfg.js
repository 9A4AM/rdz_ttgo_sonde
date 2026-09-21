var cfgs = [
[ "", "General configuration", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/General-configuration" ],
[ "wifi", "Wifi mode (0=off, 1=client, 2=AP, 3=client or AP, 4=client-noscan, 5=client or AP+retry)" ],
[ "mdnsname", "Network mDNS name"],
[ "ephftp", "FTP server for ephemeris data (RS92 decoder)"],
[ "debug", "Debug level (0=err/1=warn/2=info/3=all;+10=color)" ],
[ "maxsonde", "Maximum number of QRG entries (must be &leq; 50)" ],
[ "rxlat", "Receiver fixed latitude"],
[ "rxlon", "Receiver fixed longitude"],
[ "rxalt", "Receiver fixed altitude"],
[ "b2mute", "Button 2/medium press mutes LED/Buzzer (minutes)"],
[ "cachesize", "Offline upload cache: buffered frames for backfill after a network/WiFi outage (0=disabled, default 120)"],
[ "", "OLED/TFT display configuration", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Display-configuration" ],
[ "screenfile", "Screen config (0=automatic; 1-5=predefined; other=custom)" ],
[ "display", "Display screens (scan, default, ...)" ],
[ "dispsaver", "Display saver (0=never/1=always/2=ifnorx [+10*n: after n sec.])" ],
[ "dispcontrast", "OLED contrast (-1=use default; 0..255=set contrast)" ],
[ "norx_timeout", "No-RX-timeout in seconds (-1=disabled)"],
[ "tft_orient", "TFT orientation (0/1/2/3), OLED flip: 3"],
[ "", "Spectrum display configuration", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Spectrum-configuration" ],
[ "spectrum", "Show spectrum on start (-1=no, 0=forever, >0=time [sec])" ],
[ "startfreq", "Start frequency (MHz, decimals allowed e.g. 400.2; sweep covers this +6 MHz; default 400)" ],
[ "channelbw", "Bandwidth (kHz)" ],
[ "marker", "Spectrum MHz marker" ],   // maybe remove, assume always ==1?
[ "noisefloor", "Spectrum noisefloor" ],
[ "scanplotint", "Web scan-plot idle-sweep interval [sec] (0=disabled)" ],
[ "scan_smooth", "Scan RSSI averaging 0..7 (samples=2^(n+1)); -1=display default" ],
[ "scan_addwait", "Scan extra per-bin settle time [us]; -1=display default" ],
[ "scan_iter", "Scan sweeps per scan (1..20, default 6; more = catch periodic signals)" ],
[ "", "Auto-scan (peak detection; ignores the frequency list when enabled)" ],
[ "autoscan_enable", "Auto-scan enable (1=find sondes on spectrum peaks, 0=use frequency list)" ],
[ "autoscan_snr", "Auto-scan: min SNR above noise floor [dB] (default 10)" ],
[ "autoscan_mindist", "Auto-scan: min distance between peaks [Hz] (default 1000)" ],
[ "autoscan_quant", "Auto-scan: quantize peaks to [Hz] (default 10000)" ],
[ "autoscan_maxpeaks", "Auto-scan: max peaks per sweep (default 10)" ],
[ "autoscan_dwell", "Auto-scan: detection time per peak [sec], split across types (default 12)" ],
[ "autoscan_typedwell", "Auto-scan: fixed decode time per type [ms]; 0=derive from dwell" ],
[ "autoscan_qrgfirst", "Auto-scan: try active frequency-list channels (configured freq+type) before spectrum peaks (default 1)" ],
[ "autoscan_exclude", "Auto-scan: comma-separated MHz to ignore as noise/birdies, e.g. 400.01,400.11 (empty=none)" ],
[ "", "Receiver configuration", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Receiver-configuration" ],
[ "freqofs", "RX frequency offset (Hz)"],
[ "lnaboost", "LNA current boost (0=default, 1=150%; useful without external LNA)"],
[ "lnagain", "External LNA gain in dB (>=0), subtracted from the reported RSSI (0=no correction)"],
[ "rs41.agcbw", "RS41 AGC bandwidth"],
[ "rs41.rxbw", "RS41 RX bandwidth"],
[ "rs92.rxbw", "RS92 RX (and AGC) bandwidth"],
[ "rs92.alt2d", "RS92 2D fix default altitude"],
[ "dfm.agcbw", "DFM AGC bandwidth"],
[ "dfm.rxbw", "DFM RX bandwidth"],
[ "m10m20.agcbw", "M10/M20 AGC bandwidth"],
[ "m10m20.rxbw", "M10/M20 RX bandwidth"],
[ "mp3h.agcbw", "MP3H AGC bandwidth"],
[ "mp3h.rxbw", "MP3H RX bandwidth"],
[ "", "KISS TNC/AXUDP/AXTCP data feed configuration", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Data-feed-configuration"],
[ "call", "Call"],
[ "passcode", "Passcode"],
[ "kisstnc.active", "KISS TNC (port 14590) (needs reboot)"],
[ "axudp.active", "AXUDP active"],
[ "axudp.host", "AXUDP host[:port]"],
[ "axudp.ratelimit", "Rate limit"],
[ "tcp.active", "APRS TCP active"],
[ "tcp.timeout", "APRS TCP timeout [s] (0=off, 25=on)"],
[ "tcp.host", "APRS TCP host[:port] (default port 14580)"],
[ "tcp.host2", "APRS TCP host2[:port]"],
[ "tcp.highrate", "Rate limit"],
[ "tcp.objcall", "APRS object call"],
[ "tcp.beaconsym", "APRS tracker symbol"],
[ "tcp.chase", "APRS location reporting (0=off, 1=fixed, 2=chase/GPS, 3=auto)"],
[ "tcp.comment", "APRS location comment"],
[ "", "MQTT data feed configuration", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/MQTT-configuration"],
[ "mqtt.active", "MQTT message selection bitfield: 1=Sondes, 2=Uptime, 4=PMU, 8=GPS, 16=Scanner/Spectrum Peak, 128=Debug. 0 to disable MQTT (needs reboot)"],
[ "mqtt.id", "MQTT client ID"],
[ "mqtt.host", "MQTT server hostname"],
[ "mqtt.port", "MQTT port"],
[ "mqtt.username", "MQTT username"],
[ "mqtt.password", "MQTT password"],
[ "mqtt.prefix", "MQTT prefix"],
[ "mqtt.report_interval", "MQTT reporting interval (ms)"],
[ "", "Chasemapper settings", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Chasemapper-configuration"],
[ "cm.active", "Chasemapper active (0=disabled, 1=active)"],
[ "cm.host", "Chasemapper UDP host"],
[ "cm.port", "Chasemapper UDP port"],
[ "", "Sondeseeker settings", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Sondeseeker-configuration"],
[ "ss.active", "Sondeseeker active (0=disabled, 1=active)"],
[ "ss.host", "Sondeseeker UDP host"],
[ "ss.port", "Sondeseeker UDP port"],
[ "", "Sonde notifications (ntfy)", "" ],
[ "notify.active", "ntfy alerts: 0=disabled, 1=sonde landing near me, 2=new sonde detected, 3=both"],
[ "notify.dist", "Landing alert when sonde is within this horizontal distance [km]"],
[ "notify.alt", "Landing alert only when sonde altitude is below this [km]"],
[ "notify.server", "ntfy server base URL, HTTP only - no TLS in firmware (e.g. http://ntfy.sh)"],
[ "notify.topic", "ntfy topic to subscribe to (auto-generated random topic on first boot if left empty; sent over plain HTTP, so anyone on the path can read it)"],
[ "notify.token", "ntfy access token for protected topics (optional)"],
[ "", "SondeHub settings", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/SondeHub-settings"],
[ "sondehub.active", "SondeHub reporting (0=disabled, 1=active)"],
[ "sondehub.chase", "SondeHub location reporting (0=off, 1=fixed, 2=chase/GPS, 3=auto)"],
[ "sondehub.host", "SondeHub host (DO NOT CHANGE)"],
[ "sondehub.callsign", "Callsign"],
[ "sondehub.antenna", "Antenna (optional, visisble on SondeHub tracker)"],
[ "sondehub.email", "SondeHub email (optional, only used to contact in case of upload errors)"],
[ "", "SondeHub frequency import", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/SondeHub-import" ],
[ "sondehub.fiactive", "SondeHub frequency import active (0=disabled, 1=active)" ],
[ "sondehub.fiinterval", "Import frequency (minutes, &geq; 5)" ],
[ "sondehub.fimaxdist", "Import maximum distance (km, &leq; 700)" ],
[ "sondehub.fimaxage", "Import maximum age (hours, &leq; 48)" ],
[ "", "SD card logger configuration", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/SDcard-configuration"],
[ "sd.cs", "SD card CS" ],
[ "sd.miso", "SD card MISO/DI" ],
[ "sd.mosi", "SD card MOSI/DO" ],
[ "sd.clk", "SD card CLK" ],
[ "sd.sync", "SD card sync interval [s]" ],
[ "sd.name", "SD card naming (0=flat, 1=YYMM folders)" ],
[ "sd.speed", "SD card SPI speed (Hz, 0=default, e.g. 4000000, 8000000, 16000000)" ],
[ "", "Serial position output (Serial1 / UART1, requires reboot)"],
[ "serialout.format", "Serial output format (0=off, 1=NMEA, 2=JSON, 3=CSV, 4=PAYLOAD_SUMMARY)"],
[ "serialout.txd", "Serial output TX pin (-1 to disable; must be free & output-capable, not 6-11/34-39)"],
[ "serialout.baud", "Serial output baud rate (e.g. 9600)"],
[ "", "Hardware configuration (requires reboot)", "https://github.com/dl9rdz/rdz_ttgo_sonde/wiki/Hardware-configuration"],
[ "disptype", "Display type (0=OLED/SSD1306, 1=ILI9225, 2=OLED/SH1106, 3=ILI9341, 4=ILI9342, 5=ST7789, 6=ST7796)"],
[ "oled_sda", "OLED SDA/TFT SDA"],
[ "oled_scl", "OLED SCL/TFT CLK"],
[ "oled_rst", "OLED RST/TFT RST (needs reboot)"],
[ "tft_rs", "TFT RS"],
[ "tft_cs", "TFT CS"],
[ "tft_spifreq", "TFT SPI speed"],
[ "button_pin", "Button input port"],
[ "button2_pin", "Button 2 input port"],
[ "button2_axp", "Use AXP192 PWR as Button 2"],
[ "touch_thresh", "Touch button threshold<br>(0 for calib mode)"],
[ "power_pout", "Power control port"],
[ "led_pout", "LED output port"],
[ "gps_rxd", "GPS RXD pin (-1 to disable)"],
[ "gps_txd", "GPS TXD pin (optional, only for GSP reset)"],
[ "batt_adc", "Battery measurement pin"],
[ "sx1278_ss", "SX1278 SS"],
[ "sx1278_miso", "SX1278 MISO"],
[ "sx1278_mosi", "SX1278 MOSI"],
[ "sx1278_sck", "SX1278 SCK"],
];

var tocheck = ["sd.cs", "sd.miso", "sd.mosi", "sd.clk", "oled_sda", "oled_scl", "oled_rst", "tft_rs", "tft_cs", "tft_spifreq", "button_pin", "button2_pin", 
  "led_pout", "gps_rxd", "gps_txd", "batt_adc", "sx1278_ss", "sx1278_miso", "sx1278_mosi", "sx1278_sck"];
var alloweddups = [ ["sd.mosi", "oled_sda"], ["sd.clk", "oled_scl" ] ];

function isAllowedDup(nameA, nameB) {
   for (var i = 0; i < alloweddups.length; i++) {
       var pair = alloweddups[i];
       if ((pair[0] === nameA && pair[1] === nameB) || (pair[0] === nameB && pair[1] === nameA)) {
           return true;
       }
   }
   return false;
}
// Function to check for duplicate pins
function checkForDuplicates(form) {
    // Create an object to store values and their associated descriptions and names
    var valuesMap = {};
    var duplicates = [];

    // Iterate through the tocheck array
    for (var i = 0; i < tocheck.length; i++) {
        var inputName = tocheck[i];
        var inputValue = parseInt(document.getElementsByName(inputName)[0].value, 10);

        // Skip empty values or values that are -1
        if (isNaN(inputValue) || inputValue === -1) {
            continue;
        }

        var cfg = cfgs.find(item => item[0] === inputName);
        var descriptionB = cfg ? cfg[1] : "";

        // Check if the value is already in the map
        if (valuesMap[inputValue]) {
            var existingEntry = valuesMap[inputValue];
            var nameA = existingEntry.name;
            var descriptionA = existingEntry.description;

            // Check if the duplicate is allowed
            if (!isAllowedDup(nameA, inputName)) {
                duplicates.push({ value: inputValue, descA: descriptionA, descB: descriptionB });
            }
        } else {
            // Otherwise, store the value with its description and name
            valuesMap[inputValue] = { name: inputName, description: descriptionB };
        }
    }

    // If duplicates were found, show the warning popup
    if (duplicates.length > 0) {
        var message = "Duplicated PIN assignments found:\n";
        for (var j = 0; j < duplicates.length; j++) {
            message += "Pin " + duplicates[j].value + " in '" + duplicates[j].descA + "' and '" + duplicates[j].descB + "'\n";
        }

        // Confirm asynchronously: block this submit, and re-submit programmatically if the
        // user accepts. form.submit() does not re-fire onsubmit, so there is no loop.
        showConfirm(message + "\nDo you want to submit the form anyway?").then(function(ok) {
            if (ok) form.submit();
        });
        return false;
    }

    // No duplicates: allow the form submission to proceed.
    return true;
}

function mkcfg(id, key, label, value) {
 var s = "<tr style=\"visibility: collapse;\" class=\"cfgpanel\"><td>" + label + "</td><td><input name=\"" + key + "\" type=\"text\" value=\"" + value + "\"/></td></tr>\n";
 return s;
}
function mkcfgbtn(id, key, label, value) {
  var touch = "";
  var v = value;
  if(v != -1 && (v&128)) {
    touch = " checked";
    v = v & 127;
  }
  var s = "<tr style=\"visibility:collapse\" class=\"cfgpanel\"><td>" + label + "</td><td><input name=\"" + key + "\" type=\"text\" size=\"3\" value=\"" + v + "\"/>";
  s += "<input type=\"checkbox\" name=\"" + key + "#\" "+touch+"> Touch</td></tr>\n";
  return s;
}

function mksep(id,label,url) {
  return "<tr class=\"cfgheader\"><th class=\"cfg\" align=\"left\" colspan=\"2\">"+label+" <a href=\""+url+"\" target=\”_blank\">[wiki]</a></th></tr>\n";
}
function rowdisp(id,disp) {
  var matches = document.querySelectorAll("tr."+id);
  matches.forEach(function(e) { if(disp) e.hidden=true; else e.removeAttribute('hidden');});
  hid=id; nid="N"+id;
  if(!disp) { hid=nid; nid=id; }
  document.querySelector("span."+hid).hidden=true;
  document.querySelector("span."+nid).removeAttribute('hidden');
}
function configTable() {
  // iterate over cfgs
  var tab = "<table width=\"100%\"><tr><th>Option</th><th>Value</th></tr>\n";
  var id=0;
  for(i=0; i<cfgs.length; i++) { 
    var key = cfgs[i][0];
    var lbl = cfgs[i][1];
    if(key) {
        if(key=="button_pin" || key=="button2_pin") {
	  tab += mkcfgbtn("s"+id, key, lbl, cf.get(key));
	} else if (key=="display") {
          tab += mkcfg("s"+id, key, lbl, cf.get(key));
	  tab += "<tr style=\"visibility:collapse\" class=\"cfgpanel\"><td>"+scr+"</td><td></td></tr>"
	} else {
          tab += mkcfg("s"+id, key, lbl, cf.get(key));
	}
    } else {
        id++;
        tab += mksep("s"+id, lbl, cfgs[i][2]);
    }
  }
  tab += "</table>";
  var cfgdiv = document.getElementById("cfgtab");
  cfgdiv.innerHTML = tab;
  // enable collapse / expand of items below a header
  var acc = document.getElementsByClassName("cfgheader");
  for(i=0; i<acc.length; i++) {
    acc[i].firstChild.innerHTML = "[+] " + acc[i].firstChild.innerHTML;
    acc[i].addEventListener("click", function(e) {
      if(e.target.nodeName=="A") return;
      achar = "[+]";
      if(this.classList.toggle("active")) achar = "[\u2212]";
      this.firstChild.innerHTML = achar + this.firstChild.innerHTML.substring(3);
      var panel = this;
      console.log(panel);
      while( panel = panel.nextElementSibling) {
        console.log(panel);
	if ( panel.className!="cfgpanel") { break; }
        if(panel.style.visibility==="collapse") {
          panel.style.visibility="visible";
        } else {
          console.log("none");
          panel.style.visibility="collapse";
        } 
      }
    });
  }
  acc[0].click();
}
