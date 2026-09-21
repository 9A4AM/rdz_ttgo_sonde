#!/usr/bin/env python3
"""
mock_sondehub.py - a tiny local stand-in for the SondeHub V2 telemetry API,
for testing this firmware's offline upload cache without touching the real
service. Includes a live browser dashboard.

The firmware talks plain HTTP on port 80 (no TLS) to `sondehub.host`, and its
DNS resolver "returns immediately if the host is an IP". This mock defaults to
port 8080 for the API (no sudo needed) and serves a live dashboard on 8081.
Put a reverse proxy on :80 in front of the API, since the firmware hardcodes 80.

  1. Run the mock (unprivileged):
         python3 scripts/mock_sondehub.py        # API :8080, dashboard :8081
  2. Expose the API on :80 with nginx (the firmware always connects to :80):
         server {
             listen 80;
             location / {
                 proxy_pass http://127.0.0.1:8080;
                 proxy_http_version 1.1;
                 proxy_request_buffering off;   # stream the chunked PUT through
             }
         }
     (Or run the API directly on :80 with sudo: `sudo python3 ... --port 80`.)
  3. In the device web config set:
         sondehub.active = 1
         sondehub.host   = <your dev machine's LAN IP>   (an IP, so no DNS)
         cachesize       = 120   (enable the offline cache)
  4. Open the dashboard: http://<dev machine>:8081/  (live frames + status).
     The terminal logs every frame too.
     If you front the dashboard with an HTTPS reverse proxy, the /events SSE
     stream must NOT be buffered or it hangs on "connecting...". This mock sends
     `X-Accel-Buffering: no` (which nginx honors), but for other proxies disable
     response buffering explicitly, e.g. nginx:
         location / {
             proxy_pass http://127.0.0.1:8081;
             proxy_http_version 1.1;
             proxy_buffering off;           # stream SSE frames straight through
             proxy_read_timeout 1h;         # keep the long-lived stream open
         }

Testing the cache / backfill:
  - Live frames arrive with time_received ~= now.
  - Simulate an internet outage WITHOUT restarting: press ENTER here to toggle
    OUTAGE mode. In outage the server drops incoming connections, so the device
    can't upload and buffers frames in RAM. Press ENTER again to restore; the
    device reconnects and BACKFILLS the buffered frames in order. Backfilled
    frames are flagged "BACKFILL +Ns" (old time_received) in the log and
    highlighted on the dashboard.
  - (Or just Ctrl-C to stop and re-run to simulate a longer outage.)

Endpoints (API port):
  PUT /sondes/telemetry   chunked JSON array of frames  -> 200 {}
  PUT /listeners          station info (Content-Length) -> 200 {}
  GET /sondes?...         frequency-import query         -> 200 []  (no sondes)
  anything else                                          -> 200 {}

Python 3 stdlib only.
"""

import argparse
import collections
import datetime
import http.server
import json
import queue
import signal
import socketserver
import sys
import threading
import time

# ---- shared state -----------------------------------------------------------

class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.frames = 0
        self.telemetry_puts = 0
        self.listener_puts = 0
        self.serials = set()
        # Duplicate detection: per serial, the set of frame numbers already logged.
        # Re-received (serial, frame#) pairs are counted and flagged so a resend /
        # backfill-of-already-sent bug is unmistakable in the log.
        self.seen = {}
        self.duplicates = 0

STATS = Stats()
OUTAGE = threading.Event()   # set() => simulate "no internet" (drop connections)
# set() => read+log the telemetry PUT normally, then drop the connection WITHOUT
# sending the HTTP response. Simulates "server processed the batch but the ACK was
# lost" (upstream-fail-after-processing) — the condition that makes a client/proxy
# retransmit an already-accepted batch, so we can see whether a duplicate results.
RESP_DROP = threading.Event()

# A frame is flagged "backfill" only if its time_received is older than this many
# seconds when the batch PUT lands. Must stay ABOVE the firmware's batch window
# (conn-sondehub.cpp SONDEHUB_MAXAGE, currently 15s) plus transmit/ACK margin:
# the oldest frame of every *normal* live batch is ~15-16s old at flush time, so a
# threshold of 15 flags 1-2 frames on every batch. 25s cleanly separates normal
# batch latency (~16s) from real outage recovery (tens of seconds to minutes).
BACKFILL_THRESHOLD_S = 25


class FrameFeed:
    """In-memory history + fan-out to live dashboard (SSE) subscribers."""
    def __init__(self, history=500):
        self.lock = threading.Lock()
        self.history = collections.deque(maxlen=history)
        self.subs = set()

    def publish(self, evt):
        with self.lock:
            if evt.get("kind") == "frame":
                self.history.append(evt)
            dead = []
            for q in self.subs:
                try:
                    q.put_nowait(evt)
                except queue.Full:
                    dead.append(q)
            for q in dead:
                self.subs.discard(q)

    def subscribe(self):
        q = queue.Queue(maxsize=2000)
        with self.lock:
            snapshot = list(self.history)
            self.subs.add(q)
        return q, snapshot

    def unsubscribe(self, q):
        with self.lock:
            self.subs.discard(q)

FEED = FrameFeed()


def now_utc():
    return datetime.datetime.now(datetime.timezone.utc)


def ts():
    return time.strftime("%H:%M:%S")


def publish_status():
    FEED.publish({"kind": "status", "online": not OUTAGE.is_set(), "arrival": ts()})


# ---- buffered socket reader (handles mixed \r\n / \n line endings) -----------

class Reader:
    def __init__(self, conn):
        self.conn = conn
        self.buf = b""

    def _fill(self):
        try:
            data = self.conn.recv(4096)
        except OSError:
            return False
        if not data:
            return False
        self.buf += data
        return True

    def read_line(self):
        """Return one line as bytes (without trailing \\n / \\r), or None on close."""
        while b"\n" not in self.buf:
            if not self._fill():
                if self.buf:
                    line, self.buf = self.buf, b""
                    return line.rstrip(b"\r")
                return None
        i = self.buf.index(b"\n")
        line = self.buf[:i]
        self.buf = self.buf[i + 1:]
        return line.rstrip(b"\r")

    def read_exact(self, n):
        while len(self.buf) < n:
            if not self._fill():
                break
        data = self.buf[:n]
        self.buf = self.buf[n:]
        return data


def read_chunked(reader):
    """Read an HTTP/1.1 chunked body, return the reassembled bytes."""
    chunks = []
    while True:
        size_line = reader.read_line()
        if size_line is None:
            break
        if size_line == b"":
            continue
        try:
            size = int(size_line.split(b";")[0], 16)
        except ValueError:
            break
        if size == 0:
            reader.read_line()   # trailing CRLF after the terminating chunk
            break
        chunks.append(reader.read_exact(size))
        reader.read_line()       # trailing CRLF after chunk data
    return b"".join(chunks)


# ---- API request handling (firmware-facing) ---------------------------------

def log_frames(body, peer=""):
    try:
        frames = json.loads(body.decode("utf-8", "replace"))
    except (ValueError, UnicodeDecodeError) as e:
        print(f"[{ts()}] telemetry: could not parse JSON ({e}); {len(body)} bytes")
        return
    if isinstance(frames, dict):
        frames = [frames]
    if not isinstance(frames, list):
        print(f"[{ts()}] telemetry: unexpected JSON shape: {type(frames).__name__}")
        return
    arrival = now_utc()
    # Per-PUT accounting so a resend of an already-uploaded batch is obvious: which
    # peer sent it, how big, the frame-number span, and how many frames in THIS PUT
    # were already logged by an earlier PUT.
    nums = []
    batch_new = batch_dup = 0
    for fr in frames:
        if not isinstance(fr, dict):
            continue
        serial = fr.get("serial", "?")
        num = fr.get("frame", "?")
        typ = fr.get("type", "?")
        tr = fr.get("time_received", "")
        lat = fr.get("lat"); lon = fr.get("lon"); alt = fr.get("alt")
        if isinstance(num, int):
            nums.append(num)
        backfill = None
        try:
            trdt = datetime.datetime.strptime(tr, "%Y-%m-%dT%H:%M:%S.%fZ")
            trdt = trdt.replace(tzinfo=datetime.timezone.utc)
            delta = (arrival - trdt).total_seconds()
            if delta > BACKFILL_THRESHOLD_S:
                backfill = int(delta)
        except (ValueError, TypeError):
            pass
        # duplicate check: have we logged this exact (serial, frame#) before?
        with STATS.lock:
            seen = STATS.seen.setdefault(serial, set())
            is_dup = num in seen
            if is_dup:
                STATS.duplicates += 1
                batch_dup += 1
            else:
                seen.add(num)
                batch_new += 1
            STATS.frames += 1
            STATS.serials.add(serial)
        pos = ""
        if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
            pos = f" @ {lat:.4f},{lon:.4f}"
            if isinstance(alt, (int, float)):
                pos += f",{alt:.0f}m"
        flag = f"   <<< BACKFILL +{backfill}s" if backfill else ""
        if is_dup:
            flag += "   <<< DUPLICATE (already uploaded)"
        print(f"[{ts()}] TELEM  {typ:<5} {serial}  frame={num}  rx={tr}{pos}{flag}")
        FEED.publish({
            "kind": "frame", "arrival": ts(), "type": typ, "serial": serial,
            "frame": num, "time_received": tr, "lat": lat, "lon": lon, "alt": alt,
            "backfill": backfill, "duplicate": is_dup,
        })
    span = f"{min(nums)}..{max(nums)}" if nums else "-"
    dupflag = f"  <<< {batch_dup} DUPLICATE" if batch_dup else ""
    print(f"[{ts()}] PUT  from {peer}  {len(body)}B  frames={len(frames)} "
          f"span={span}  new={batch_new} dup={batch_dup}{dupflag}")


class ApiHandler(socketserver.BaseRequestHandler):
    def handle(self):
        conn = self.request
        peer = f"{self.client_address[0]}:{self.client_address[1]}"

        if OUTAGE.is_set():
            try:
                conn.close()   # simulate "no internet": drop the connection
            except OSError:
                pass
            return

        conn.settimeout(30)
        reader = Reader(conn)
        while True:
            if OUTAGE.is_set():
                break
            req_line = reader.read_line()
            if req_line is None:
                break
            if req_line == b"":
                continue
            parts = req_line.decode("latin-1").split()
            method = parts[0] if parts else ""
            path = parts[1] if len(parts) > 1 else "/"

            headers = {}
            while True:
                line = reader.read_line()
                if line is None:
                    return
                if line == b"":
                    break
                k, _, v = line.partition(b":")
                headers[k.strip().lower()] = v.strip()

            # Prefer the real client IP forwarded by nginx (X-Real-IP, else the first
            # hop in X-Forwarded-For) over the TCP peer, which is nginx itself.
            xri = headers.get(b"x-real-ip", b"")
            xff = headers.get(b"x-forwarded-for", b"")
            if xri:
                client = xri.decode("latin-1").strip()
            elif xff:
                client = xff.decode("latin-1").split(",")[0].strip()
            else:
                client = peer

            te = headers.get(b"transfer-encoding", b"").lower()
            if b"chunked" in te:
                body = read_chunked(reader)
            elif b"content-length" in headers:
                try:
                    n = int(headers[b"content-length"])
                except ValueError:
                    n = 0
                body = reader.read_exact(n)
            else:
                body = b""

            if path.startswith("/sondes/telemetry"):
                with STATS.lock:
                    STATS.telemetry_puts += 1
                log_frames(body, client)
                if RESP_DROP.is_set():
                    print(f"[{ts()}] >>> RESP_DROP: logged batch, dropping WITHOUT ACK")
                    try:
                        conn.close()
                    except OSError:
                        pass
                    return
                self.respond(conn, b"{}")
            elif path.startswith("/listeners"):
                with STATS.lock:
                    STATS.listener_puts += 1
                print(f"[{ts()}] STATION listener update from {client} ({len(body)} bytes)")
                FEED.publish({"kind": "station", "arrival": ts(), "peer": client})
                self.respond(conn, b"{}")
            elif method == "GET" and path.startswith("/sondes"):
                self.respond(conn, b"[]")   # freq-import query -> no sondes in range
            else:
                self.respond(conn, b"{}")

    @staticmethod
    def respond(conn, body, status="200 OK"):
        head = (
            f"HTTP/1.1 {status}\r\n"
            f"Content-Type: application/json\r\n"
            f"Content-Length: {len(body)}\r\n"
            f"Connection: keep-alive\r\n\r\n"
        ).encode("ascii")
        try:
            conn.sendall(head + body)
        except OSError:
            pass


class ApiServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


# ---- live dashboard (browser-facing) ----------------------------------------

DASHBOARD_HTML = """<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>mock SondeHub - live</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; background:#0e1116; color:#d7dde5;
         font:14px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }
  header { display:flex; align-items:center; gap:18px; flex-wrap:wrap;
           padding:12px 18px; background:#161b22; border-bottom:1px solid #262c36; position:sticky; top:0; }
  h1 { font-size:15px; margin:0; font-weight:600; letter-spacing:.3px; color:#e8eef5; }
  .pill { padding:3px 10px; border-radius:999px; font-weight:600; font-size:12px;
          border:1px solid transparent; }
  .pill.online  { background:#0f2e1c; color:#3fb950; border-color:#1f6f3f; }
  .pill.outage  { background:#3a1113; color:#f85149; border-color:#8a2b2b; }
  .pill.offline { background:#21262d; color:#8b949e; border-color:#30363d; }
  .stats { display:flex; gap:16px; margin-left:auto; flex-wrap:wrap; }
  .stat { text-align:right; }
  .stat b { display:block; font-size:18px; color:#e8eef5; font-weight:700; }
  .stat span { font-size:11px; color:#8b949e; text-transform:uppercase; letter-spacing:.5px; }
  table { width:100%; border-collapse:collapse; }
  thead th { position:sticky; top:53px; background:#12161c; text-align:left;
             padding:7px 12px; font-size:11px; text-transform:uppercase; letter-spacing:.5px;
             color:#8b949e; border-bottom:1px solid #262c36; }
  td { padding:5px 12px; border-bottom:1px solid #1a1f27; white-space:nowrap; }
  tbody tr:hover { background:#141922; }
  tr.backfill { background:#2a2113; }
  tr.backfill:hover { background:#332815; }
  .badge { background:#8a6d1f; color:#ffe9a8; padding:1px 7px; border-radius:6px; font-size:11px; font-weight:700; }
  .type { color:#79c0ff; }
  .muted { color:#6e7681; }
  .empty { padding:40px; text-align:center; color:#6e7681; }
</style></head>
<body>
<header>
  <h1>mock SondeHub</h1>
  <span id="pill" class="pill offline">connecting...</span>
  <div class="stats">
    <div class="stat"><b id="cFrames">0</b><span>frames</span></div>
    <div class="stat"><b id="cSerials">0</b><span>serials</span></div>
    <div class="stat"><b id="cBackfill">0</b><span>backfilled</span></div>
    <div class="stat"><b id="cPuts">0</b><span>telem puts</span></div>
  </div>
</header>
<table>
  <thead><tr><th>arrival</th><th>type</th><th>serial</th><th>frame</th>
    <th>time_received</th><th>position</th><th></th></tr></thead>
  <tbody id="rows"><tr><td class="empty" colspan="7">waiting for frames...</td></tr></tbody>
</table>
<script>
  const rows = document.getElementById('rows');
  const pill = document.getElementById('pill');
  let frames = 0, backfilled = 0, puts = 0, connected = false;
  const serials = new Set();
  let empty = true;

  function setPill() {
    if (!connected) { pill.className = 'pill offline'; pill.textContent = 'server offline'; return; }
    if (window._online === false) { pill.className = 'pill outage'; pill.textContent = 'OUTAGE (device buffering)'; }
    else { pill.className = 'pill online'; pill.textContent = 'ONLINE'; }
  }
  function num(x){ return (typeof x === 'number') ? x : null; }
  function esc(s){ const d=document.createElement('div'); d.textContent = (s==null?'':String(s)); return d.innerHTML; }

  function addFrame(m) {
    if (empty) { rows.innerHTML = ''; empty = false; }
    frames++; serials.add(m.serial); if (m.backfill) backfilled++;
    document.getElementById('cFrames').textContent = frames;
    document.getElementById('cSerials').textContent = serials.size;
    document.getElementById('cBackfill').textContent = backfilled;
    let pos = '';
    const lat = num(m.lat), lon = num(m.lon), alt = num(m.alt);
    if (lat !== null && lon !== null) {
      pos = lat.toFixed(4) + ',' + lon.toFixed(4) + (alt !== null ? ',' + Math.round(alt) + 'm' : '');
    }
    const tr = document.createElement('tr');
    if (m.backfill) tr.className = 'backfill';
    tr.innerHTML =
      '<td class="muted">' + esc(m.arrival) + '</td>' +
      '<td class="type">' + esc(m.type) + '</td>' +
      '<td>' + esc(m.serial) + '</td>' +
      '<td>' + esc(m.frame) + '</td>' +
      '<td class="muted">' + esc(m.time_received) + '</td>' +
      '<td>' + esc(pos) + '</td>' +
      '<td>' + (m.backfill ? '<span class="badge">BACKFILL +' + m.backfill + 's</span>' : '') + '</td>';
    rows.insertBefore(tr, rows.firstChild);
    while (rows.childNodes.length > 300) rows.removeChild(rows.lastChild);
  }

  function connect() {
    const es = new EventSource('/events');
    es.onopen = () => { connected = true; setPill(); };
    es.onerror = () => { connected = false; setPill(); };
    es.onmessage = (e) => {
      let m; try { m = JSON.parse(e.data); } catch (_) { return; }
      if (m.kind === 'status') { window._online = m.online; setPill(); }
      else if (m.kind === 'frame') { addFrame(m); }
    };
  }
  connect();
</script>
</body></html>
"""


class DashHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass  # keep the dashboard quiet in the terminal

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/events":
            self.serve_events()
        else:
            self.serve_html()

    def serve_html(self):
        body = DASHBOARD_HTML.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_events(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        # Behind a reverse proxy (nginx), proxy_buffering holds the stream so the
        # browser's EventSource never fires onopen and stays stuck "connecting...".
        # This header tells nginx to disable buffering for this response.
        self.send_header("X-Accel-Buffering", "no")
        self.end_headers()
        q, snapshot = FEED.subscribe()

        def send(evt):
            self.wfile.write(("data: " + json.dumps(evt) + "\n\n").encode("utf-8"))
            self.wfile.flush()

        try:
            send({"kind": "status", "online": not OUTAGE.is_set(), "arrival": ts()})
            for evt in snapshot:
                send(evt)
            while True:
                try:
                    send(q.get(timeout=15))
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            FEED.unsubscribe(q)


class DashServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True


# ---- outage toggle via stdin ------------------------------------------------

def toggle_outage():
    if OUTAGE.is_set():
        OUTAGE.clear()
        print(f"\n[{ts()}] >>> ONLINE  — accepting uploads (device will backfill)\n")
    else:
        OUTAGE.set()
        print(f"\n[{ts()}] >>> OUTAGE  — dropping connections (device will buffer)\n")
    publish_status()


def stdin_toggle_loop():
    for _ in iter(sys.stdin.readline, ""):
        toggle_outage()


# Toggle OUTAGE via signal too, so the mock can be driven when launched in the
# background with no controlling TTY:  kill -USR1 <pid>
def _sigusr1_toggle(*_):
    toggle_outage()


# SIGUSR2: toggle RESP_DROP (log batch, then drop without ACK) — see RESP_DROP.
def _sigusr2_toggle(*_):
    if RESP_DROP.is_set():
        RESP_DROP.clear()
        print(f"\n[{ts()}] >>> RESP_DROP OFF — acking normally\n")
    else:
        RESP_DROP.set()
        print(f"\n[{ts()}] >>> RESP_DROP ON — will log batches then drop without ACK\n")


def print_summary(*_):
    with STATS.lock:
        print("\n---- mock_sondehub summary ----")
        print(f"  telemetry PUTs : {STATS.telemetry_puts}")
        print(f"  listener  PUTs : {STATS.listener_puts}")
        print(f"  frames logged  : {STATS.frames}")
        print(f"  unique serials : {len(STATS.serials)}"
              + (f"  ({', '.join(sorted(str(s) for s in STATS.serials))})" if STATS.serials else ""))
    sys.exit(0)


def main():
    ap = argparse.ArgumentParser(description="Mock SondeHub telemetry API + live dashboard")
    ap.add_argument("--host", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    ap.add_argument("--port", type=int, default=8080,
                    help="API bind port (default 8080; front it with nginx on :80, "
                         "or use --port 80 with sudo)")
    ap.add_argument("--web-port", type=int, default=8081,
                    help="dashboard bind port (default 8081; 0 disables the dashboard)")
    args = ap.parse_args()

    try:
        sys.stdout.reconfigure(line_buffering=True)  # type: ignore[attr-defined]
    except AttributeError:
        pass
    signal.signal(signal.SIGINT, print_summary)
    signal.signal(signal.SIGTERM, print_summary)
    signal.signal(signal.SIGUSR1, _sigusr1_toggle)
    signal.signal(signal.SIGUSR2, _sigusr2_toggle)

    try:
        api = ApiServer((args.host, args.port), ApiHandler)
    except PermissionError:
        print(f"error: binding {args.host}:{args.port} needs privileges — run with "
              f"sudo, or keep the default 8080 and front it with nginx.", file=sys.stderr)
        sys.exit(1)
    except OSError as e:
        print(f"error: cannot bind API {args.host}:{args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    if args.web_port:
        try:
            dash = DashServer((args.host, args.web_port), DashHandler)
        except OSError as e:
            print(f"error: cannot bind dashboard {args.host}:{args.web_port}: {e}", file=sys.stderr)
            sys.exit(1)
        threading.Thread(target=dash.serve_forever, daemon=True).start()
        print(f"dashboard: http://<this-host>:{args.web_port}/")

    print(f"mock SondeHub API listening on {args.host}:{args.port}")
    print("point the device's sondehub.host at this machine's IP (an IP, not a name).")
    print("press ENTER to toggle OUTAGE (buffer) / ONLINE (backfill); Ctrl-C for summary.\n")

    threading.Thread(target=stdin_toggle_loop, daemon=True).start()
    try:
        api.serve_forever()
    except KeyboardInterrupt:
        print_summary()


if __name__ == "__main__":
    main()
