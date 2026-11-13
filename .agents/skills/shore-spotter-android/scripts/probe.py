#!/usr/bin/env python3
"""Read-only Shore Spotter probe; screenshots require its own foreground window."""
import argparse
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request

PACKAGE = "tw.shorespotter.app"


def station_origin(value):
    match = re.fullmatch(r"([A-Za-z0-9.-]+)(?::([0-9]+))?", value.strip())
    if not match:
        raise ValueError("Use a private Station IP or .local name, without a protocol or path")
    host, port = match.groups()
    if port and not 1 <= int(port) <= 65535:
        raise ValueError("Invalid Station port")
    local = host.lower().endswith(".local") and len(host) > 6
    if not local:
        addr = ipaddress.ip_address(host)
        local = any(addr in ipaddress.ip_network(net) for net in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))
    if not local:
        raise ValueError("Station must be in the local private network")
    return "http://" + host.lower() + (":" + port if port else "")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def get_json(origin, route):
    with urllib.request.build_opener(NoRedirect()).open(origin + route, timeout=3) as response:
        raw = response.read(65537)
    if len(raw) > 65536:
        raise ValueError("Station response exceeds capacity")
    return json.loads(raw)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", required=True, help="Explicit authorized adb serial or IP:port")
    parser.add_argument("--adb", help="Path to adb, if it is not on PATH")
    parser.add_argument("--station-ip", help="Optional read-only Station API inspection; never POST")
    parser.add_argument("--screenshot", type=Path, help="New local file; refuses another app or lock screen")
    args = parser.parse_args()
    sdk_adb = Path(os.environ.get("ANDROID_HOME", str(Path.home() / "Android/Sdk"))) / "platform-tools/adb"
    adb = args.adb or shutil.which("adb") or (str(sdk_adb) if sdk_adb.is_file() else None)
    if not adb:
        parser.error("adb not found; use --adb or ANDROID_HOME")
    prefix = [adb, "-s", args.device]

    def shell(*command, check=True):
        return subprocess.run(prefix + ["shell", *command], capture_output=True, text=True, timeout=15, check=check).stdout.strip()

    def own_foreground():
        windows = shell("dumpsys", "window")
        focus = [line for line in windows.splitlines() if "mCurrentFocus=" in line]
        return any(re.search(r"(?<![\w.])tw\.shorespotter\.app(?:/|[\s}])", line) for line in focus)

    subprocess.run(prefix + ["get-state"], capture_output=True, timeout=10, check=True)
    report = {"package": PACKAGE, "model": shell("getprop", "ro.product.model"),
              "android": shell("getprop", "ro.build.version.release"),
              "api": shell("getprop", "ro.build.version.sdk"),
              "installed": shell("pm", "path", PACKAGE, check=False).startswith("package:"),
              "own_app_foreground": own_foreground()}
    if args.screenshot:
        if not report["own_app_foreground"]:
            raise RuntimeError("Shore Spotter is not the foreground window; no screenshot captured")
        data = subprocess.run(prefix + ["exec-out", "screencap", "-p"], capture_output=True, timeout=15, check=True).stdout
        if not own_foreground():
            raise RuntimeError("Focus changed; screenshot discarded without saving")
        if not data.startswith(b"\x89PNG\r\n\x1a\n"):
            raise RuntimeError("Device did not return a PNG")
        # Exclusive creation protects existing evidence and user files.
        with args.screenshot.open("xb") as output:
            output.write(data)
        args.screenshot.chmod(0o600)
        report["screenshot"] = str(args.screenshot.resolve())
    if args.station_ip:
        base = station_origin(args.station_ip)
        track = get_json(base, "/api/track")
        control = get_json(base, "/api/client/control")
        position = get_json(base, "/api/station/position")
        servo = track.get("servo", {})
        report["station"] = {"hostname": track.get("hostname"), "servo": {
            key: servo.get(key) for key in ("mode", "angle", "target", "moving", "output_ready", "gps_available")},
            "client": {key: control.get(key) for key in ("client_id", "supported", "state", "can_control", "command", "station_boot_id", "operation_id")},
            "position": {key: position.get(key) for key in ("source", "valid", "accuracy_m", "age_ms")}}
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
