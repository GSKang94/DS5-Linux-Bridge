#!/usr/bin/env python3
"""
TV control server for DS5-Linux-Bridge.
The Pico sends HTTP requests here, and this script runs the adb commands.

Usage: python3 tv_server.py
Listens on port 7777.

Endpoints:
  GET /tv/sleep   - Put TV to sleep
  GET /tv/input   - Switch TV to HDMI 1
  GET /tv/wake    - Wake TV (WoL) + switch input
  GET /tv/status  - Health check
"""

import subprocess
import http.server
import time

TV_IP = "192.168.2.128"
TV_MAC = "38:26:56:62:AB:8F"

# The command that switches to HDMI 1 on your TCL TV
INPUT_CMD = (
    "am start -a android.intent.action.VIEW "
    "-d 'content://android.media.tv/passthrough/"
    "com.tcl.tvinput%2F.TvPassThroughService%2FHW15' "
    "-n com.tcl.tv/com.tcl.player.TVActivity -f 0x10008000"
)

def adb(cmd):
    """Run an adb shell command on the TV."""
    try:
        result = subprocess.run(
            ["adb", "-s", f"{TV_IP}:5555", "shell", cmd],
            capture_output=True, text=True, timeout=10
        )
        print(f"  [ADB] shell {cmd}")
        print(f"  [ADB] rc={result.returncode} stdout={result.stdout.strip()} stderr={result.stderr.strip()}")
        return result.returncode == 0
    except subprocess.TimeoutExpired:
        print(f"  [ADB] shell TIMEOUT: {cmd}")
        return False

def ensure_connected():
    """Make sure adb is connected to the TV."""
    try:
        result = subprocess.run(
            ["adb", "connect", f"{TV_IP}:5555"],
            capture_output=True, text=True, timeout=5
        )
        connected = "connected" in result.stdout
        print(f"  [ADB] connect -> {'OK' if connected else 'FAILED'} ({result.stdout.strip()})")
        return connected
    except subprocess.TimeoutExpired:
        print(f"  [ADB] connect -> TIMEOUT")
        return False

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        print(f"\n[REQ] {self.path} from {self.client_address[0]}")

        if self.path == "/tv/sleep":
            print("[TV] Sleep")
            ensure_connected()
            ok = adb("input keyevent 223")
        elif self.path == "/tv/input":
            print("[TV] Switch input")
            ensure_connected()
            ok = adb(INPUT_CMD)
        elif self.path == "/tv/wake":
            print("[TV] Wake + input")
            print(f"  [WOL] Sending magic packet to {TV_MAC}")
            subprocess.run(["wakeonlan", TV_MAC], capture_output=True)
            print(f"  [WOL] Waiting 5s for TV to boot...")
            time.sleep(5)
            if not ensure_connected():
                print("  [ADB] Not ready, retrying in 3s...")
                time.sleep(3)
                ensure_connected()
            ok = adb(INPUT_CMD)
        elif self.path == "/tv/status":
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"ok")
            return
        else:
            self.send_response(404)
            self.end_headers()
            return

        print(f"[RESULT] {'OK' if ok else 'FAILED'}")
        self.send_response(200 if ok else 500)
        self.end_headers()
        self.wfile.write(b"ok" if ok else b"fail")

    def log_message(self, format, *args):
        pass  # Suppress default HTTP logs, we have our own

if __name__ == "__main__":
    print(f"TV control server on :7777 (TV={TV_IP})")
    print(f"  GET /tv/sleep  - sleep TV")
    print(f"  GET /tv/input  - switch to HDMI 1")
    print(f"  GET /tv/wake   - WoL + switch input")
    print(f"  GET /tv/status - health check")
    http.server.HTTPServer(("", 7777), Handler).serve_forever()
