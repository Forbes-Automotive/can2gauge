#!/usr/bin/env python3
"""
Records the simulator's answers to every endpoint the web UI calls into can2gauge_api.json, the mock API
readme_shots.py serves (api_include in can2gauge.json). So the README's web UI shots show a running gauge: the sim's
Mk4 warm and on boost, the Bluetooth adapter paired with the VIN read, the car's DBC loaded, a K-line session open,
the adc2can on the bus, the OpenHaldex heard over WiFi, and usb2ip sending an Ignitron's channels (page 6 shows three
of them: Source = ECU).

    py tools/readme_shots/can2gauge_api.py      then
    py tools/readme_shots.py tools/readme_shots/can2gauge.json

Standard library only (plus the sim itself).
"""
import gzip
import json
import os
import socket
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "sim"))

ENDPOINTS = [
    "/api/status", "/api/signals", "/api/themes", "/api/settings", "/api/haldex", "/api/adc", "/api/oil", "/api/dsg",
    "/api/elm", "/api/chain", "/api/can/recent", "/api/can/ids", "/api/can/dbc", "/api/kline/block", "/api/me7/vars",
    "/api/me7read/status", "/api/files", "/api/sprites", "/api/ota/info", "/api/ota/check", "/api/ota/fsinfo", "/api/wifi/sta",
    "/api/wifi/scan", "/api/firstboot", "/api/service", "/api/vdiag", "/api/vehicle", "/dbc/index.json", "/dbc/fingerprints.json", "/api/ecu", "/api/ecu/library?kind=1",
    "/api/drag",  # 0.38.0: the drag timer's scoreboard (Log tab)
] + ["/api/ecu/library?kind=1&group=%d&page=%d" % (g, page) for g in range(6) for page in range(2)]  # usb2ip's Ignitron
# channels, a group at a time and 100 to a page (the biggest Ignitron group has 140)

# the bench car held at a good moment: warm, on boost, Haldex locking (same as glass_shots.py PIN_PULL)
PINS = {"rpm": 4650, "speed": 96, "map_mbar": 2180, "pedal": 78, "coolant": 91, "oil": 104, "iat": 31,
        "battery": 14.0, "hdx_actual": 72, "fuel": 41}


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def get(port, path):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), headers={"Accept-Encoding": "gzip"})
    with urllib.request.urlopen(req, timeout=10) as r:
        body = r.read()
        if r.headers.get("Content-Encoding") == "gzip":
            body = gzip.decompress(body)
        return r.headers.get("Content-Type", ""), body


LOG_API = {}  # /api/log and /api/log/data, filled by record_log


def record_log(port, sim):
    """the data logger (0.16.0): waits for the engine to run, skips to the end of idle so a full-throttle pull comes
    next, records 26 s at 10 Hz, then keeps /api/log and the whole recording (binary, base64) for the mock API"""
    import base64
    t0 = time.time()
    while time.time() - t0 < 15 and not sim.car.running:
        time.sleep(0.2)
    with sim.lock:
        sim.car.phase_i, sim.car.phase_t = 2, 12.0  # 3 s more idle, then "accelerate"
        sim.settings["log"]["rateHz"] = 10
        sim.logger.start()
    print("recording a pull for the Log tab - 26 s")
    time.sleep(26)
    with sim.lock:
        sim.logger.stop()
    LOG_API["/api/log"] = json.loads(get(port, "/api/log")[1].decode("utf-8"))
    body = get(port, "/api/log/data?from=0&max=5000")[1]
    LOG_API["/api/log/data"] = {"_b64": base64.b64encode(body).decode("ascii"), "_type": "application/octet-stream"}
    print("  ok    %-26s %6d bytes" % ("/api/log/data", len(body)))


def main():
    import server
    port = free_port()
    httpd = server.serve("127.0.0.1", port)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    sim = server.SIM
    record_log(port, sim)  # the Log tab's recording: a pull from the drive cycle, before the car is pinned below
    with sim.lock:
        c = sim.car
        c.cfg.update(scenario="manual", pedal=60, dsg=True, dsgSel="D")
        c.coolant, c.oil, c.iat = 90.0, 98.0, 28.0
        c.overrides.update({k: float(v) for k, v in PINS.items()})
        sim.settings["elm"]["enabled"] = 1
        sim.settings["learnt"]["elmMac"] = "a4:c1:38:5e:12:7b"
        sim.settings["oil"]["source"] = 1  # K-line: temperature + level
        sim.settings["kline"]["modeAddr"] = 1
        for ms in (5240, 4910, 5630, 6020, 4450):  # 0.38.0: drag timer runs for the Log tab's scoreboard
            sim.drag_add(0, ms)
        # page 6: an ECU page from usb2ip - knock retard on the arc, injector duty / lambda target / IAT in the centre stack
        sim.apply_settings({"pages": [{}, {}, {}, {}, {}, {"enabled": 1, "style": 9, "source": "ecu", "signal": "ecu:1:136",
                                                            "label": "KNOCK #1", "min": 0, "max": 15, "redline": 6, "decimals": 1,
                                                            "slots": ["ecu:1:204", "ecu:1:188", "ecu:1:76"]}]})
        sim.restart_kline = True
        text = gzip.open(os.path.join(ROOT, "data", "dbc", "vw_pq.c2d.gz"), "rt", encoding="utf-8").read()
        sim.can_dbc_load(text)
    print("sim on :%d - warming up 20 s (adapter pairs, K-line wakes, the oil level arrives)" % port)
    t0 = time.time()
    while time.time() - t0 < 20:  # the web UI's K-line card polls this, which keeps the browse session open
        try:
            get(port, "/api/kline/block")
        except OSError:
            pass
        time.sleep(0.5)
    # 0.17.5: K-line mode's live block, kept now while its session is open (the fault read below takes the line), and
    # the module's label file as text for the web UI's Find... list
    import base64
    kline_block = json.loads(get(port, "/api/kline/block")[1].decode("utf-8"))
    LOG_API["/api/kline/block"] = kline_block
    print("  ok    %-26s connected=%s block %s" % ("/api/kline/block", kline_block.get("connected"), kline_block.get("group")))
    if kline_block.get("labelFile"):
        body = get(port, "/api/lbl?file=" + kline_block["labelFile"])[1]
        LOG_API["/api/lbl"] = {"_b64": base64.b64encode(body).decode("ascii"), "_type": "text/plain; charset=utf-8"}
        print("  ok    %-26s %6d bytes (%s)" % ("/api/lbl", len(body), kline_block["labelFile"]))
    # 0.20.0: the ME7 logger's .ecu and .cfg as the web UI fetches them (/download, URL-encoded) - so the variable
    # checkbox picker (K-line tab) and the gauge-page ME7 sources have their variable list in the mock
    import urllib.parse
    for me7path in ("/me7/sample.ecu", "/me7/log_example.cfg"):
        body = get(port, "/download?path=" + me7path)[1]
        key = "/download?path=" + urllib.parse.quote(me7path, safe="")  # matches encodeURIComponent() in app.js
        LOG_API[key] = {"_b64": base64.b64encode(body).decode("ascii"), "_type": "text/plain; charset=utf-8"}
        print("  ok    %-26s %6d bytes" % ("/download " + me7path, len(body)))
    # the Service tab: a fault read of the engine over the K-line (the sim's ME7 answers after the 5-baud wake-up)
    with sim.lock:
        sim.service.start({"job": 1, "via": 0, "addr": 1})
    time.sleep(3.5)
    # 0.20.0: the fault read took the K-line, so let the ME7 logger reconnect and settle into "logging" before the
    # endpoints below record /api/me7/vars and /api/status (so the ME7 shot shows a running logger, not "connecting")
    for _ in range(16):
        try:
            if json.loads(get(port, "/api/me7/vars")[1].decode("utf-8")).get("state") == "logging":
                break
        except OSError:
            pass
        time.sleep(0.5)
    api = dict(LOG_API)
    for ep in ENDPOINTS:
        if ep in api:
            continue  # already kept above (K-line mode's block while its session was open)
        try:
            ctype, body = get(port, ep)
        except OSError as e:
            print("  skip  %-26s %s" % (ep, e))
            continue
        try:
            api[ep] = json.loads(body.decode("utf-8"))
        except ValueError:
            print("  skip  %-26s not JSON (%s)" % (ep, ctype))
            continue
        print("  ok    %-26s %6d bytes" % (ep, len(body)))
    out = os.path.join(HERE, "can2gauge_api.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(api, f, separators=(",", ":"))
    print("wrote %s (%d endpoints)" % (os.path.relpath(out, ROOT), len(api)))


if __name__ == "__main__":
    main()
