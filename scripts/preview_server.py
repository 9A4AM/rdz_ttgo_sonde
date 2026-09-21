#!/usr/bin/env python3
"""Local preview server for the rdzTTGOSonde web UI (RX_FSK/data/).

Serves the LittleFS data/ folder over HTTP so the web interface can be viewed
on a desktop without flashing an ESP32. It substitutes the device-side
%PLACEHOLDER% template tokens with sample values and mocks the JSON endpoints
(/users.json, /status.json) that the pages fetch from the firmware.

Note: pages whose tabs load device-generated HTML via iframe (config.html,
status.html, qrg.html, ...) are NOT available locally -- those are produced by
the firmware's processor() at runtime, not stored as static files.

Usage:
    python3 scripts/preview_server.py [--port 8099] [--root RX_FSK/data]
"""
import argparse
import json
import math
import os
import random
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

PLACEHOLDERS = {
    "%VERSION_NAME%": "rdzTTGOSonde",
    "%VERSION_ID%": "devel20260615",
    "%FULLNAMEID%": "teste20260616-C3",
    "%ALLOWFILEUPLOAD%": "1",   # show the Update-from-file section in preview
    "%AUTODETECT_INFO%": "TTGO LoRa32 v2.1 (auto-detected)",
    "%LOCAL_UPDATES%": "",
    "%MAPCENTER%": "48.0,11.0,10",
    "%PREAUTH%": "0123456789abcdef",
    "%BOOTID%": "preview-boot-1",
}

# Fallback placeholders for pages the firmware generates through processor() at runtime
# (see sendConfig/sendQRG/... in RX_FSK.ino) -- they are NOT static files, so without an
# entry here the iframe tabs would 404 during local preview. Empty because every
# device-generated page has a faithful preview renderer below.
DEVICE_GENERATED = {}

# Control page: reproduce the buttons the firmware's createControlForm() emits, so the
# real layout/styling can be previewed locally (must match RX_FSK.ino).
#
# The first 8 buttons (key1/key2 short/double/medium/long) are labelled dynamically from
# the current screen's actions[1..8] -- mirror that here. Action codes are the ACT_* values
# in src/Sonde.h; action_descr() mirrors actionDescr() in RX_FSK.ino.
ACT_NONE = 255
ACT_DISPLAY_SCANNER = 0
ACT_FORMAT_SD = 59
ACT_RINEX_UPDATE = 60
ACT_DISPLAY_WIFI = 61
ACT_DISPLAY_SPECTRUM = 62
ACT_DISPLAY_DEFAULT = 63
ACT_DISPLAY_NEXT = 64
ACT_NEXTSONDE = 65
ACT_PREVSONDE = 66
ACT_MAXDISPLAY = 50

_ACT_LABELS = {
    ACT_NONE: "no function",
    ACT_DISPLAY_SCANNER: "scanner",
    ACT_DISPLAY_WIFI: "WiFi screen",
    ACT_DISPLAY_SPECTRUM: "spectrum",
    ACT_DISPLAY_DEFAULT: "default screen",
    ACT_DISPLAY_NEXT: "next screen",
    ACT_NEXTSONDE: "next frequency",
    ACT_PREVSONDE: "previous frequency",
    ACT_RINEX_UPDATE: "update RINEX",
    ACT_FORMAT_SD: "format SD card",
}


def action_descr(act):
    if act in _ACT_LABELS:
        return _ACT_LABELS[act]
    return ("screen %d" % act) if act < ACT_MAXDISPLAY else ("action %d" % act)


# Sample current-screen key actions[1..8] (key1 then key2, each short/double/medium/long),
# matching a typical default OLED layout, so the dynamic labels can be previewed.
SAMPLE_ACTIONS = [ACT_NEXTSONDE, ACT_DISPLAY_SCANNER, ACT_DISPLAY_SPECTRUM, ACT_DISPLAY_WIFI,
                  ACT_DISPLAY_NEXT, ACT_NONE, ACT_NONE, ACT_NONE]
_KP_NAME = ["short", "double", "medium", "long"]
CONTROL_KEYIDS = ["rx", "scan", "spec", "wifi", "rx2", "scan2", "spec2", "wifi2"]
# Static (non-keypress) controls. "format" is admin-only (level 2) in the firmware.
CONTROL_EXTRA = [
    ("rinex", "Update RS92 RINEX eph", 1),
    ("format", "Format SD Card", 2),
    ("reboot", "Reboot", 1),
]


_HEAD = ('<!DOCTYPE html><html><head><meta charset="UTF-8">'
         '<meta name="color-scheme" content="light dark">'
         '<script src="theme.js"></script>'
         '<link rel="stylesheet" type="text/css" href="style.css"></head>')


def _footer(version_id, save=True, extra=""):
    s = '</div><div class="footer"><div class="footer-left">'
    if save:
        s += '<input type="submit" class="save" value="Save changes"/>'
    s += extra
    s += '</div><span class="ttgoinfo">rdzTTGOserver ' + version_id + '</span></div>'
    return s


# Match the firmware's SVG_BACKUP (floppy/save) and SVG_RESTORE (folder + up arrow) icons.
_SVG_OPEN = ('<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" '
             'stroke-width="2" stroke-linecap="round" stroke-linejoin="round">')
_SVG_SAVE = (_SVG_OPEN +
             '<path d="M19 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h11l5 5v11a2 2 0 0 1-2 2z"/>'
             '<polyline points="17 21 17 13 7 13 7 21"/><polyline points="7 3 7 8 15 8"/></svg>')
_SVG_RESTORE = (_SVG_OPEN +
                '<path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/>'
                '<polyline points="9 13 12 10 15 13"/><line x1="12" y1="10" x2="12" y2="16"/></svg>')


def _backup_footer(idd, fname, label):
    # Simplified version of QRG_/CONFIG_BACKUP_FOOTER (download/save + restore icon buttons).
    # The file input wires uploadCfgFile() onchange, matching the device, so the
    # restore-then-reboot flow (progress dialog + reload) is testable in preview.
    return (
        '<input type="file" id="%s" accept=".txt" style="display:none" '
        'onchange="uploadCfgFile(\'%s\',\'%s\',\'%s\')">' % (idd, idd, fname, label) +
        '<span class="bkpbtns">'
        '<a class="iconbtn" href="/file/%s" title="Download backup (%s)">%s</a>' % (fname, fname, _SVG_SAVE) +
        '<button type="button" class="iconbtn" title="Restore from file (%s)" '
        'onclick="document.getElementById(\'%s\').click()">%s</button>' % (label, idd, _SVG_RESTORE) +
        '</span>'
    )


def render_wifi_page(version_id):
    rows = ""
    sample = [("rdzSonde-AP", ""), ("MyHomeWiFi", "secret123"), ("Field-Hotspot", "balloon42")]
    for i in range(10):
        ssid, pw = sample[i] if i < len(sample) else ("", "")
        nr = "<b>AP</b>" if i == 0 else str(i)
        rows += ('<tr><td>%s</td><td><input name="S%d" type="text" value="%s"/></td>'
                 '<td><input name="P%d" type="text" value="%s"/></td></tr>'
                 % (nr, i + 1, ssid, i + 1, pw))
    return (
        _HEAD + '<body><form class="wrapper" action="wifi.html" method="post"><div class="content">'
        '<table><tr><th>Nr</th><th>SSID</th><th>Password</th></tr>' + rows + '</table>'
        '<script>footer()</script>' + _footer(version_id, save=True) +
        '</form></body></html>'
    ).replace("<head>", '<head><script src="rdz.js"></script>', 1)


def render_qrg_page(version_id):
    sample = [
        '[1, "403.000", "Munich", "4"]', '[1, "405.700", "Stuttgart", "4"]',
        '[0, "404.000", "Lindenberg", "R"]', '[1, "402.500", "Test M20", "M"]',
        '[0, "404.500", "DFM site", "D"]',
    ]
    qrgs = "".join("qrgs.push(%s);\n" % s for s in sample)
    return (
        _HEAD.replace("<head>",
                      '<head><script src="rdz.js"></script><script src="dialog.js"></script>', 1) +
        '<body><form class="wrapper" action="qrg.html" method="post"><div class="content">'
        '<script>\nvar qrgs = [];\n' + qrgs + '</script>'
        '<div id="divTable"></div><script> qrgTable() </script>'
        + _footer(version_id, save=True, extra=_backup_footer("qrgupl", "qrg.txt", "frequency list"))
        + '</form></body></html>'
    )


def render_status_page(version_id):
    def sonde(id_, ser, lat, lon):
        return (
            '<table class="stat"><tr><td id="caption">%s</td></tr>'
            '<tr><td>Frame# 4242, Sats=9, 2026-06-16 12:34:56</td></tr>'
            '<tr><td><a href="geo:%f,%f">GEO-App</a> - '
            '<a href="https://radiosondy.info/sonde_archive.php?sondenumber=%s">radiosondy.info</a> - '
            '<a href="https://tracker.sondehub.org/%s">SondeHub Tracker</a></td></tr></table>'
            % (id_, lat, lon, id_, ser)
        )
    body = sonde("V1234567", "V1234567", 48.13, 11.57) + sonde("W7654321", "W7654321", 48.20, 11.40)
    return (
        _HEAD.replace("</head>", '<meta http-equiv="refresh" content="5"></head>', 1) +
        '<body><form class="wrapper" action="status.html" method="post"><div class="content">'
        + body
        + _footer(version_id, save=False) + '</form></body></html>'
    )


def render_config_page(version_id):
    scr = ("Using /screens1.txt<br>0=Scanner<br>1=Legacy<br>2=Field<br>3=Field2"
           "<br>4=GPSDIST<br>5=BatteryOLED<br>6=Meteo<br>7=GPS-Data<br>8=ScannerBatt")
    # cfg.js only ever calls cf.get(key); a plain object with .get is enough. A few sample
    # values make it look realistic; everything else falls back to "".
    sample = ('{"mdnsname":"rdzsonde","sondehub.callsign":"MYCALL","sondehub.active":"1",'
              '"screenfile":"1","display":"0,1,2,3,4","maxsonde":"6","norx_timeout":"20"}')
    return (
        _HEAD.replace("<head>",
                      '<head><script src="rdz.js"></script><script src="dialog.js"></script>', 1) +
        '<body><form class="wrapper" action="config.html" method="post" '
        'onsubmit="return checkForDuplicates(this)"><div class="content">'
        '<div id="cfgtab"></div><script src="cfg.js"></script>'
        '<script>\nvar scr="' + scr + '";\n'
        'var _s=' + sample + ';\nvar cf={get:function(k){return (k in _s)?_s[k]:"";}};\n'
        'configTable();\nfooter();\n</script>'
        + _footer(version_id, save=True, extra=_backup_footer("cfgupl", "config.txt", "configuration"))
        + '</form></body></html>'
    )


def render_control_page(version_id, level=2):
    btns = ""
    for i, name in enumerate(CONTROL_KEYIDS):
        act = SAMPLE_ACTIONS[i]
        descr = action_descr(act)
        # Function first, then which button/keypress triggers it; capitalize the first letter.
        label = "%s (button %d %s keypress)" % (
            descr[:1].upper() + descr[1:], 1 if i < 4 else 2, _KP_NAME[i & 3])
        dis = " disabled" if act == ACT_NONE else ""  # no function -> grey it out
        btns += ('<input class="ctlbtn" type="submit" name="%s" value="%s"%s></input>'
                 % (name, label, dis))
        if i == 3 or i == 7:
            btns += "<p></p>"
    for name, label, minlevel in CONTROL_EXTRA:
        if level < minlevel:
            continue  # e.g. Format SD Card is hidden below admin (level 2)
        # Mirror createControlForm(): guard the disruptive buttons with the styled confirmation.
        onclick = ""
        if name == "format":
            onclick = (" onclick=\"return confirmSubmit(this,'Format the SD card?"
                       "\\nThis permanently erases all data on the card.');\"")
        elif name == "reboot":
            onclick = " onclick=\"return confirmReboot('Reboot the device now?');\""
        btns += ('<input class="ctlbtn" type="submit" name="%s" value="%s"%s></input>'
                 % (name, label, onclick))
    # confirmSubmit / confirmReboot mirror the inline script createControlForm() emits: the
    # Reboot path captures /bootid, fires the reboot POST and waits for the device to come back
    # via waitForRebootAndReload() (dialog.js), so the reload flow is testable in preview.
    script = (
        '<script src="dialog.js"></script><script>'
        'function confirmSubmit(b,msg){showConfirm(msg).then(function(ok){if(!ok)return;'
        "var h=document.createElement('input');h.type='hidden';h.name=b.name;h.value=b.value;"
        'b.form.appendChild(h);b.form.submit();});return false;}'
        'function confirmReboot(msg){showConfirm(msg).then(function(ok){if(!ok)return;'
        "fetch('/bootid',{cache:'no-store'}).then(function(r){return r.ok?r.text():'';})"
        ".catch(function(){return '';}).then(function(before){"
        "fetch('/control.html',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'reboot=1'}).catch(function(){});"
        "waitForRebootAndReload((before||'').trim(),'Rebooting','The device is rebooting.');});});return false;}"
        '</script>'
    )
    return (
        '<!DOCTYPE html><html><head><meta charset="UTF-8">'
        '<meta name="color-scheme" content="light dark">'
        '<script src="theme.js"></script>'
        '<link rel="stylesheet" type="text/css" href="style.css">'
        + script +
        '</head>'
        '<body><form class="wrapper" action="control.html" method="post"><div class="content">'
        + btns +
        '</div><div class="footer"><span></span>'
        '<span class="ttgoinfo">rdzTTGOserver ' + version_id + '</span>'
        '</div></form></body></html>'
    )

STUB_TEMPLATE = """<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="color-scheme" content="light dark">
<script src="theme.js"></script>
<link rel="stylesheet" type="text/css" href="style.css"></head>
<body><div class="wrapper"><div class="content" style="padding:2em">
<h2>{title}</h2>
<p><em>This page is generated by the firmware on the ESP32 (processor() in
RX_FSK.ino) and has no static file. It can only be viewed on real hardware.</em></p>
</div></div></body></html>"""

MOCK_JSON = {
    # Effective access level of the "logged-in" preview user. Change level to 1 (or 0)
    # to preview how the nav hides the admin-only tabs (Control/Config/WiFi/Users).
    "/whoami.json": {"user": "admin", "level": 2},
    "/users.json": [
        {"user": "admin", "level": 2},
        {"user": "guest", "level": 1},
    ],
    "/status.json": {
        "TTGO": "Autodetect info: TTGO LoRa32 v2.1",
        "WiFi": "connected, IP 192.168.1.42",
        "SondeHub": "uploading (last OK 3s ago)",
        "MQTT": "disabled",
        "GPS": "fix: 48.000, 11.000",
    },
}


# Wall-clock time of the last update-triggering POST. /bootid uses it to simulate a
# device reboot a few seconds later (a changed bootid is how the update dialog detects
# completion). 0 = no update triggered yet.
_UPDATE_AT = [0.0]

# Synthetic spectrum for /spectrum.json (scanplot.html). A noisy baseline with a
# few peaks and one tall mid-band spike; seq increments so the page animates.
_SPECTRUM_SEQ = [0]


def gen_spectrum():
    # Values are dBm (matching the firmware: RSSI[dBm] = -RssiValue/2).
    n, start, step, base = 210, 400, 6.0 / 210, -110.0
    data = [base + random.uniform(-2, 2) for _ in range(n)]
    for frac, h in ((0.03, 30), (0.13, 42), (0.28, 22), (0.40, 38),
                    (0.55, 55), (0.72, 18), (0.86, 26), (0.95, 34)):
        c, amp = round(frac * n), h + random.uniform(-3, 3)
        for i in range(max(0, c - 4), min(n, c + 5)):
            d = i - c
            data[i] = max(data[i], base + amp * math.exp(-(d * d) / 2.0))
    mid = round(0.5 * n)
    data[mid] = max(data[mid], -50 + random.uniform(-2, 2))
    _SPECTRUM_SEQ[0] += 1
    return {
        "seq": _SPECTRUM_SEQ[0], "age_ms": 0, "startfreq": start, "step": step,
        "n": n, "noisefloor": -110, "peak": 403.0, "interval": 60,
        "status": "idle", "data": [round(v, 1) for v in data],
    }


def make_handler(root):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, ctype, body):
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            path = self.path.split("?")[0]
            # Drain the request body so uploads (multipart firmware/fs images) complete
            # cleanly and the client's upload-progress reaches 100%.
            remaining = int(self.headers.get("Content-Length", 0) or 0)
            while remaining > 0:
                chunk = self.rfile.read(min(remaining, 65536))
                if not chunk:
                    break
                remaining -= len(chunk)
            # These POSTs reboot the device (OTA, file-upload OTA, or an explicit reboot
            # after a config/qrg restore); record it so /bootid can simulate the device
            # coming back with a new bootid a few seconds later.
            if path in ("/update.html", "/uploadota", "/control.html"):
                _UPDATE_AT[0] = time.time()
            # auth/user endpoints: just acknowledge so the JS doesn't error
            self._send(200, "text/plain", "ok")

        def do_GET(self):
            path = self.path.split("?")[0]
            if path == "/logout":
                # Mirror the firmware: clear the session cookie and bounce to the login page.
                self.send_response(302)
                self.send_header("Location", "/login.html")
                self.send_header("Set-Cookie", "SESSION=; Path=/; Max-Age=0")
                self.end_headers()
                return
            generated = {
                "/control.html": render_control_page,
                "/qrg.html": render_qrg_page,
                "/wifi.html": render_wifi_page,
                "/status.html": render_status_page,
                "/config.html": render_config_page,
            }
            if path in generated:
                if path == "/control.html":
                    # Mirror the firmware: Format SD Card is hidden below admin level.
                    lvl = MOCK_JSON["/whoami.json"]["level"]
                    body = render_control_page(PLACEHOLDERS["%VERSION_ID%"], lvl)
                else:
                    body = generated[path](PLACEHOLDERS["%VERSION_ID%"])
                self._send(200, "text/html", body)
                return
            if path == "/spectrum.json":
                self._send(200, "application/json", json.dumps(gen_spectrum()))
                return
            # Boot nonce used by the update dialog to detect a reboot. Returns a new
            # value ~8s after an update was triggered, simulating the device rebooting.
            if path == "/bootid":
                rebooted = _UPDATE_AT[0] and (time.time() - _UPDATE_AT[0]) > 8
                self._send(200, "text/plain",
                           "preview-boot-2" if rebooted else "preview-boot-1")
                return
            # Mock the public update server's version pages so upd.html's update
            # validation can be exercised locally. Installed is teste20260616-C3
            # (see %FULLNAMEID%): main differs by letter (C->D, blocked), dev2 differs
            # by number (3->5, allowed with a "filesystem changes" note).
            if path == "/update-info.html":   # flat PY5OL build info
                self._send(200, "text/html",
                           "<html><body><p>py5ol-20260712175225-C3</p></body></html>")
                return
            if path in ("/main/update-info.html", "/dev2/update-info.html"):
                ver = "teste20260616-D3" if path.startswith("/main") else "teste20260616-C5"
                self._send(200, "text/html",
                           "<html><body><p>%s</p></body></html>" % ver)
                return
            if path in MOCK_JSON:
                self._send(200, "application/json", json.dumps(MOCK_JSON[path]))
                return
            if path in DEVICE_GENERATED:
                self._send(200, "text/html",
                           STUB_TEMPLATE.format(title=DEVICE_GENERATED[path]))
                return
            if path in ("/", ""):
                path = "/index.html"
            fp = os.path.join(root, path.lstrip("/"))
            if not os.path.isfile(fp):
                self._send(404, "text/plain", "not found: " + path)
                return
            ext = os.path.splitext(fp)[1].lower()
            ctype = {
                ".html": "text/html",
                ".css": "text/css",
                ".js": "application/javascript",
                ".json": "application/json",
                ".png": "image/png",
            }.get(ext, "text/plain")
            mode = "r" if ext in (".html", ".css", ".js", ".txt", ".json") else "rb"
            with open(fp, mode) as f:
                data = f.read()
            if ext == ".html":
                for k, v in PLACEHOLDERS.items():
                    data = data.replace(k, v)
            self._send(200, ctype, data)

    return Handler


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--root", default=os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "RX_FSK", "data"))
    args = ap.parse_args()
    srv = HTTPServer((args.host, args.port), make_handler(args.root))
    print(f"Serving {args.root} at http://{args.host}:{args.port}")
    srv.serve_forever()


if __name__ == "__main__":
    main()
