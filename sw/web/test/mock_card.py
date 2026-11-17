#!/usr/bin/env python3
"""A mock PicoGUS card for working on the web page without hardware.

usage: mock_card.py [port] [features]

Serves the page from ../www, packed the way the firmware build packs it (and
packed again on every request, so edit and reload), and answers the web
server's HTTP API with made-up values. GET /log returns the requests made.

    python3 sw/web/test/mock_card.py 8080
    python3 sw/web/test/mock_card.py 8080 gus,mpu,joy,multifw     (GUS mode)
"""
import json
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts"))
import web_pack  # noqa: E402

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
FEATURES = sys.argv[2].split(",") if len(sys.argv) > 2 else ["sb", "opl", "mpu", "joy", "cd", "multifw"]
MODE = 1 if "gus" in FEATURES else 4 if "tandy" in FEATURES else 5 if "sb" in FEATURES else 2 if "opl" in FEATURES else 3

state = {
    "fw": "picogus-sb v4.2.0-beta.2", "mode": MODE, "board": 0, "chip": "RP2040", "uptime": 3725,
    "features": FEATURES,
    "net": {"ssid": "RetroLAN", "link": 3, "rssi": -58, "ip": "192.168.1.42", "mac": "28:cd:c1:00:11:22",
            "pollMaxUs": 4120, "pollRecentUs": 830, "loadPermille": 7},
    "s": {"joy": 1, "gusport": 0x240, "gusbuf": 4, "gusdma": 0, "gus44k": 0, "sbport": 0x220, "sbirq": 5, "sbdma": 1,
          "sbtype": 6, "sbfixtc": 0, "sblockmixer": 0, "oplport": 0x388, "oplwait": 0, "mpuport": 0x330, "mpudelay": 0,
          "mpufake": 0, "mpuwait": 0, "tandyport": 0x2c0, "cmsport": 0x220, "mouseport": 0, "mouseproto": 0,
          "mouserate": 60, "mousesen": 256, "cdport": 0x250, "cdauto": 1, "mainvol": 100, "oplvol": 100, "sbvol": 90,
          "cdvol": 100, "gusvol": 100, "psgvol": 80},
    "savePending": 0, "midiFree": 1023, "midiHeld": 0,
    "upload": {"kind": 0, "state": 0, "done": 0, "total": 0, "error": ""},
}
if "cd" in FEATURES:
    state["cd"] = {"index": 2, "image": "TOMBRAID.CUE", "status": 0, "error": ""}
IMAGES = ["DOOM2.ISO", "TOMBRAID.CUE", "<b>WC3 & co</b>.CUE"] + [f"GAME{i:02}.ISO" for i in range(1, 22)]
log = []
cd_polls = [0]


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, code, obj=None, body=None, ctype="application/json"):
        data = body if body is not None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            page = web_pack.build_page(os.path.join(HERE, "..", "www")).encode("utf-8")
            self.send(200, body=page, ctype="text/html; charset=utf-8")
        elif path == "/api/status":
            state["uptime"] += 3
            self.send(200, state)
        elif path == "/api/cd":
            cd_polls[0] += 1
            if cd_polls[0] % 2:
                self.send(200, {"busy": True})
            else:
                self.send(200, {"current": state["cd"]["index"], "images": IMAGES, "truncated": False, "error": ""})
        elif path == "/log":
            self.send(200, log)
        else:
            self.send(404, {"error": "not found"})

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        path = self.path
        if path.startswith("/api/upload/"):
            log.append({"path": path, "bytes": len(body)})
            if path.endswith("syx"):
                return self.send(200, {"ok": True, "messages": body.count(b"\xf7")})
            if not body.startswith(b"UF2\n"):
                return self.send(400, {"error": "not a UF2 file"})
            return self.send(200, {"ok": True, "reboot": True})
        form = dict(urllib.parse.parse_qsl(body.decode(), keep_blank_values=True))
        log.append({"path": path, "form": form})
        if path == "/api/set":
            for key, value in form.items():
                if key not in state["s"]:
                    return self.send(400, {"error": "invalid value"})
                state["s"][key] = int(value, 0)
            return self.send(200, state)
        if path == "/api/defaults":
            state["s"].update(sbvol=90, mainvol=100, sbirq=5, sbport=0x220)
            return self.send(200, state)
        if path == "/api/mode":
            state["mode"] = int(form["mode"])
            return self.send(200, {"ok": True})
        if path == "/api/wifi":
            state["net"]["ssid"] = form.get("ssid", "")
            return self.send(200, {"ok": True})
        if path == "/api/cd/load":
            index = int(form["index"])
            state["cd"].update(index=index, image=IMAGES[index - 1] if index else "")
            return self.send(200, {"ok": True})
        if path == "/api/midi":
            hexstr = form.get("hex", "").replace(" ", "")
            if not hexstr or len(hexstr) % 2:
                return self.send(400, {"error": "invalid hex"})
            return self.send(200, {"ok": True})
        if path in ("/api/save", "/api/reboot"):
            return self.send(200, {"ok": True})
        self.send(404, {"error": "not found"})


if __name__ == "__main__":
    print(f"mock PicoGUS card on http://127.0.0.1:{PORT}/")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
