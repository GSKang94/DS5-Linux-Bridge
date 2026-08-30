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
"""

import subprocess
import http.server
import threading
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
ADB_READY_TIMEOUT = 25
ADB_CONNECT_TIMEOUT = 2
_command_lock = threading.Lock()

def adb(cmd):
    """Run an adb shell command on the TV."""
    try:
        result = subprocess.run(
            ["adb", "-s", f"{TV_IP}:5555", "shell", cmd],
            capture_output=True, text=True, timeout=10
        )
        print(f"  adb shell {cmd} -> {result.returncode}")
        return result.returncode == 0
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"  adb shell failed: {exc}")
        return False

def ensure_connected():
    """Make sure adb is connected to the TV."""
    try:
        result = subprocess.run(
            ["adb", "connect", f"{TV_IP}:5555"],
            capture_output=True, text=True, timeout=ADB_CONNECT_TIMEOUT
        )
        return "connected" in result.stdout
    except (OSError, subprocess.TimeoutExpired) as exc:
        print(f"  adb connect failed: {exc}")
        return False

def wait_for_adb():
    """Wait until the TV has completed enough of its boot to accept ADB."""
    deadline = time.monotonic() + ADB_READY_TIMEOUT
    while time.monotonic() < deadline:
        if ensure_connected():
            return True
        time.sleep(1)
    print("  adb connect timed out waiting for TV boot")
    return False

def send_wol():
    try:
        subprocess.run(["wakeonlan", TV_MAC], capture_output=True, timeout=5,
                       check=True)
        return True
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print(f"  wakeonlan failed: {exc}")
        return False

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path not in ("/tv/sleep", "/tv/input", "/tv/wake"):
            self.send_response(404)
            self.end_headers()
            return

        # The Pico can see both USB resume and mount for one physical wake.
        # Firmware coalesces them, but do the same here so an old firmware or a
        # retried TCP request cannot send a second ADB input command.
        if not _command_lock.acquire(blocking=False):
            print(f"[TV] Coalesced duplicate {self.path} from {self.client_address[0]}")
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"coalesced")
            return

        try:
            self._run_command()
        finally:
            _command_lock.release()

    def _run_command(self):
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
            ok = send_wol() and wait_for_adb() and adb(INPUT_CMD)

        self.send_response(200 if ok else 500)
        self.end_headers()
        self.wfile.write(b"ok" if ok else b"fail")

    def log_message(self, format, *args):
        print(f"[HTTP] {args[0]}")

if __name__ == "__main__":
    print(f"TV control server on :7777 (TV={TV_IP})")
    print(f"  GET /tv/sleep  - sleep TV")
    print(f"  GET /tv/input  - switch to HDMI 1")
    print(f"  GET /tv/wake   - WoL, wait for ADB, switch input")
    http.server.ThreadingHTTPServer(("", 7777), Handler).serve_forever()
