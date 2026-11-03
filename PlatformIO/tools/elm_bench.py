#!/usr/bin/env python3
"""
Run the gauge's BLE ELM bench tests (firmware 0.8.0+) from a laptop and keep the logs - one sitting with a new adapter.

    python tools/elm_bench.py                          # "all": probe, raw CAN, DTCs, TP2.0 engine + DSG module
    python tools/elm_bench.py probe raw                # just these, in this order
    python tools/elm_bench.py tp20 --module 02 --group 1
    python tools/elm_bench.py kline --module 01 --group 1  # VAG KWP2000 on the K-line + a KWP1281 wake-up check
    python tools/elm_bench.py --fetch                  # only download every /logs/elm_*.txt already on the gauge
    python tools/elm_bench.py --host 192.168.0.42      # gauge on home WiFi instead of its own AP

Before: laptop on the gauge's WiFi (AP default 192.168.1.1), adapter set up in the web UI (Bluetooth OBD tab -
Bluetooth or WiFi) and connected, car's ignition on, engine off. The tests only read (no DTC clearing / coding /
adaptation). With a WiFi adapter the laptop drops for a few seconds when the gauge joins the adapter (the gauge's WiFi
moves to the adapter's channel); the script just waits and carries on.

Each test's log is saved to bench_logs/<yyyymmdd-hhmmss>_<gauge file name>, plus a bench_logs/summary.txt line per
test. Standard library only.
"""
import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

KINDS = ("all", "probe", "raw", "dtc", "tp20", "kline")


def get_json(host, path, timeout=5):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def post_json(host, path, body, timeout=5):
    req = urllib.request.Request(f"http://{host}{path}", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except urllib.error.HTTPError as e:  # 409 = not connected / busy, with a JSON reason
        try:
            return json.loads(e.read().decode("utf-8", "replace"))
        except ValueError:
            return {"error": f"HTTP {e.code}"}


def download(host, gauge_path, out_dir, stamp):
    q = urllib.parse.urlencode({"path": gauge_path})
    for attempt in range(5):  # the gauge's WiFi can blip (channel move when it joins a WiFi adapter)
        try:
            with urllib.request.urlopen(f"http://{host}/download?{q}", timeout=20) as r:
                data = r.read()
            break
        except (urllib.error.URLError, OSError):
            if attempt == 4:
                raise
            time.sleep(3)
    dest = out_dir / f"{stamp}_{Path(gauge_path).name}"
    dest.write_bytes(data)
    return dest


def wait_ready(host, timeout_s=60):
    """Wait until the adapter is connected and no test is running (a test resets the adapter afterwards)."""
    t0 = time.time()
    last = None
    while time.time() - t0 < timeout_s:
        try:
            e = get_json(host, "/api/elm")
        except (urllib.error.URLError, OSError) as ex:
            state = f"gauge not reachable ({ex})"
        else:
            if e.get("connected") and not e["test"]["running"] and "polling" in (e.get("state") or ""):
                return e
            state = f"{e.get('state')} (connected={e.get('connected')})"
        if state != last:
            print(f"  waiting: {state}")
            last = state
        time.sleep(1)
    return None


def run_one(host, kind, module, group, out_dir, summary):
    e = wait_ready(host)
    if not e:
        print(f"[{kind}] adapter never became ready - is it paired / is the ignition on?")
        return False
    runs0 = e["test"]["runs"]
    r = post_json(host, "/api/elm/action", {"action": "test", "kind": kind, "module": module, "group": group})
    if "error" in r:
        print(f"[{kind}] refused: {r['error']}")
        return False
    print(f"[{kind}] started")
    step = None
    t0 = time.time()
    while time.time() - t0 < 300:
        time.sleep(1)
        try:
            t = get_json(host, "/api/elm")["test"]
        except (urllib.error.URLError, OSError):
            continue  # the web server is busy for a moment now and then
        if t["running"] and t["step"] != step:
            step = t["step"]
            print(f"  {step}")
        if not t["running"] and t["runs"] > runs0:
            stamp = time.strftime("%Y%m%d-%H%M%S")
            for line in t["result"].split(" | "):
                print(f"  RESULT {line}")
            if t["file"]:
                dest = download(host, t["file"], out_dir, stamp)
                print(f"  log saved: {dest}")
            with open(out_dir / "summary.txt", "a", encoding="utf-8") as f:
                f.write(f"{stamp} {kind}: {t['result']}\n")
            summary.append((kind, t["result"]))
            return True
    print(f"[{kind}] no result after 5 minutes")
    return False


def fetch_all(host, out_dir):
    files = get_json(host, "/api/files")["files"]
    logs = [f["path"] for f in files if Path(f["path"]).name.startswith("elm_")]
    stamp = time.strftime("%Y%m%d-%H%M%S")
    for p in logs:
        print(f"  {download(host, p, out_dir, stamp)}")
    print(f"{len(logs)} log(s) fetched")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tests", nargs="*", choices=KINDS, default=["all"], metavar="test",
                    help="all | probe | raw | dtc | tp20 | kline (default all)")
    ap.add_argument("--host", default="192.168.1.1", help="gauge address (default: its own AP, 192.168.1.1)")
    ap.add_argument("--module", default="01", help="module for tp20 / dtc / kline, hex (default 01 = engine)")
    ap.add_argument("--group", type=int, default=1, help="measuring block for tp20 (default 1)")
    ap.add_argument("--out", default="bench_logs", help="folder for the logs (default bench_logs)")
    ap.add_argument("--fetch", action="store_true", help="only download the elm_*.txt logs already on the gauge")
    a = ap.parse_args()

    out_dir = Path(a.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    if a.fetch:
        fetch_all(a.host, out_dir)
        return 0
    module = int(a.module, 16)
    summary = []
    ok = True
    for kind in a.tests:
        ok &= run_one(a.host, kind, module, a.group, out_dir, summary)
    print("\n==== summary")
    for kind, res in summary:
        print(f"{kind}:")
        for line in res.split(" | "):
            print(f"  {line}")
    print(f"\nlogs in {out_dir.resolve()}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
