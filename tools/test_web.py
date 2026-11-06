#!/usr/bin/env python3
"""Tests the web server of a running device (docs/ui/web/web-app.md). Needs a device with a login.

    python tools/test_web.py <device> [--user admin] [--password ...]

The password is asked for, or taken from PX_PASSWORD. Nothing is changed on the device: no
factory reset, no new login, no "log out everywhere"; it logs in and out once. Only the Python
standard library.
"""

import argparse
import base64
import concurrent.futures
import getpass
import gzip
import io
import http.client
import json
import os
import socket
import sys
import time
import urllib.parse
import zipfile

results: list[tuple[bool, str]] = []


def check(ok: bool, name: str, detail: str = "") -> None:
    results.append((ok, name))
    print(f"{'ok  ' if ok else 'FAIL'}  {name}{'  ' + detail if detail else ''}")


class Client:
    def __init__(self, host: str):
        self.host = host
        self.cookie = ""

    def request(self, method: str, path: str, body: bytes | None = None, form: dict | None = None,
                headers: dict | None = None, timeout: float = 10) -> tuple[int, dict | str, dict]:
        conn = http.client.HTTPConnection(self.host, timeout=timeout)
        # as a browser does: the web image has gzip copies only (406 without)
        headers = {"Accept-Encoding": "gzip", **(headers or {})}
        if form is not None:
            body = urllib.parse.urlencode(form).encode()
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        if self.cookie:
            headers["Cookie"] = self.cookie
        conn.request(method, path, body=body, headers=headers)
        response = conn.getresponse()
        raw = response.read()
        if response.getheader("Content-Encoding") == "gzip":
            raw = gzip.decompress(raw)
        cookie = response.getheader("Set-Cookie")
        if cookie:
            self.cookie = cookie.split(";", 1)[0]
        conn.close()
        try:
            data: dict | str = json.loads(raw)
        except ValueError:
            data = raw.decode(errors="replace")
        return response.status, data, dict(response.getheaders())


def idle_connections_test(device: str, count: int = 20) -> None:
    """Browsers keep connections open; one that goes away without closing them (a laptop that
    sleeps, a phone that leaves the network) leaves them open on the device. More of them than the
    device has sockets must not lock out a new client (hassctrl, 2026-10-31: 11 of them left by a
    page, every accept failed with ENFILE until Wi-Fi reconnected)."""
    idle: list[socket.socket] = []
    refused = 0
    for _ in range(count):
        try:
            s = socket.create_connection((device, 80), timeout=5)
            s.sendall(f"GET /api/session HTTP/1.1\r\nHost: {device}\r\nConnection: keep-alive\r\n\r\n".encode())
            answer = s.recv(4096)
            if not answer.startswith(b"HTTP/1.1 200"):
                raise OSError(answer[:40])
            idle.append(s)
        except OSError:
            refused += 1
    time.sleep(1)
    try:
        status, _, _ = Client(device).request("GET", "/api/session", timeout=10)
    except OSError as e:
        status = f"{type(e).__name__} {e}"
    check(status == 200, f"a new client while {count} connections stay open",
          f"{status}, {len(idle)} answered, {refused} refused")
    for s in idle:
        s.close()
    time.sleep(1)
    try:
        status, _, _ = Client(device).request("GET", "/api/session", timeout=10)
    except OSError as e:
        status = f"{type(e).__name__} {e}"
    check(status == 200, "a new client after they closed", str(status))


def read_answer(sock: socket.socket) -> bytes:
    """One whole answer on a kept connection: the headers, then Content-Length bytes of body."""
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = sock.recv(4096)
        if not chunk:
            return data
        data += chunk
    head, body = data.split(b"\r\n\r\n", 1)
    length = next((int(line.split(b":", 1)[1]) for line in head.split(b"\r\n")
                   if line.lower().startswith(b"content-length:")), 0)
    while len(body) < length:
        chunk = sock.recv(4096)
        if not chunk:
            break
        body += chunk
    return head + b"\r\n\r\n" + body


def idle_close_test(device: str) -> None:
    """A session silent for 30 s is closed by the device; one with a request every 10 s stays."""
    request = f"GET /api/session HTTP/1.1\r\nHost: {device}\r\nConnection: keep-alive\r\n\r\n".encode()
    silent = []
    for _ in range(5):
        s = socket.create_connection((device, 80), timeout=5)
        s.sendall(request)
        read_answer(s)
        silent.append(s)
    busy = socket.create_connection((device, 80), timeout=5)
    busy_ok = True
    for _ in range(4):
        busy.sendall(request)
        busy_ok = busy_ok and read_answer(busy).startswith(b"HTTP/1.1 200")
        time.sleep(10)
    closed = 0
    for s in silent:
        s.settimeout(2)
        try:
            closed += s.recv(16) == b""
        except OSError:
            closed += 1
        s.close()
    check(closed == len(silent), "silent sessions are closed after 30 s", f"{closed} of {len(silent)} after 40 s")
    try:
        busy.sendall(request)
        busy_ok = busy_ok and read_answer(busy).startswith(b"HTTP/1.1 200")
    except OSError:
        busy_ok = False
    busy.close()
    check(busy_ok, "a session with a request every 10 s stays open")


class LiveSocket:
    """A minimal WebSocket client (RFC 6455) for /api/live (text frames) and /api/leds/live (binary
    frames, `binary()`), standard library only."""

    def __init__(self, device: str, cookie: str, path: str = "/api/live"):
        self.sock = socket.create_connection((device, 80), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET {path} HTTP/1.1\r\nHost: {device}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                           + (f"Cookie: {cookie}\r\n" if cookie else "") + "\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1024)
            if not chunk:
                break
            head += chunk
        self.status = int(head.split(b" ", 2)[1]) if head.startswith(b"HTTP/") else 0
        self.buffer = head.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in head else b""

    def send(self, text: str) -> None:
        data = text.encode()
        mask = os.urandom(4)
        self.sock.sendall(bytes([0x81, 0x80 | len(data)]) + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n: int) -> bytes:
        while len(self.buffer) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed by the device")
            self.buffer += chunk
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def receive(self, timeout: float, heartbeat: bool = False) -> dict | None:
        """The next text frame as JSON, None when none came within `timeout` s. The heartbeat `{}`
        is skipped unless `heartbeat`."""
        deadline = time.time() + timeout
        while True:
            message = self._frame(max(0.01, deadline - time.time()))
            if message is None or message != {} or heartbeat:
                return message

    def binary(self, timeout: float) -> bytes | None:
        """The next binary frame, None when none came within `timeout` s."""
        self.sock.settimeout(timeout)
        try:
            first, second = self._read(2)
        except socket.timeout:
            return None
        length = second & 0x7F
        if length == 126:
            length = int.from_bytes(self._read(2), "big")
        elif length == 127:
            length = int.from_bytes(self._read(8), "big")
        payload = self._read(length)
        if first & 0x0F == 0x8:
            raise ConnectionError("closed by the device")
        return payload if first & 0x0F == 0x2 else self.binary(timeout)

    def _frame(self, timeout: float) -> dict | None:
        self.sock.settimeout(timeout)
        try:
            first, second = self._read(2)
        except socket.timeout:
            return None
        length = second & 0x7F
        if length == 126:
            length = int.from_bytes(self._read(2), "big")
        elif length == 127:
            length = int.from_bytes(self._read(8), "big")
        payload = self._read(length)
        if first & 0x0F == 0x8:
            raise ConnectionError("closed by the device")
        return json.loads(payload) if first & 0x0F == 0x1 else self._frame(timeout)

    def close(self) -> None:
        self.sock.close()


def preview_test(device: str, cookie: str, features: list[str]) -> None:
    """The LED preview socket: a plain one gets the canvas alone (4 bytes width and height, then
    RGB), one with ?audio (the Sound page) the sound alone (34 bytes), never pixels."""
    if "leds" not in features:
        print("  (no LEDs: preview socket skipped)")
        return
    plain = LiveSocket(device, cookie, "/api/leds/live")
    start = time.time()
    frames = [plain.binary(2) for _ in range(6)]
    took = time.time() - start
    plain.close()
    got = [f for f in frames if f is not None]
    sizes = [len(f) for f in got]

    def canvas(f: bytes) -> bool:
        width, height = f[0] | f[1] << 8, f[2] | f[3] << 8
        return width > 0 and height > 0 and len(f) == 4 + width * height * 3

    check(len(got) == 6 and all(canvas(f) for f in got), "the preview socket sends the canvas alone", str(sizes))
    # 10 a second (5 for a large canvas): 6 take at least 0.4 s, sound frames between would not
    check(took >= 0.4, "the canvas at most 10 a second", f"{took:.2f} s for 6")
    sound = LiveSocket(device, cookie, "/api/leds/live?audio")
    frames = [sound.binary(1) for _ in range(10)]
    sound.close()
    if "audio" in features:
        got = [f for f in frames if f is not None]
        check(len(got) >= 8 and all(len(f) == 34 for f in got),
              "the sound-only socket sends 34 byte frames, 20 a second", str([len(f) for f in got][:10]))
    else:
        check(all(f is None for f in frames), "the sound-only socket sends nothing without audio")


def live_test(device: str, cookie: str, user: str, password: str) -> None:
    """The web app's live socket (docs/ui/web/web-app.md section 4.1)."""
    live = LiveSocket(device, "")
    check(live.status == 401, "the live socket without login is 401", str(live.status))
    live.close()

    live = LiveSocket(device, cookie)
    check(live.status == 101, "the live socket opens with the login", str(live.status))
    live.send("/api/status")
    first = live.receive(3)
    check(first is not None and first.get("path") == "/api/status", "the state comes at once",
          str(first)[:80])
    second = live.receive(5)
    uptimes = [m["state"]["device"]["uptime_s"] for m in (first, second) if m]
    check(len(uptimes) == 2 and uptimes[1] > uptimes[0], "a change comes by itself (uptime)", str(uptimes))
    _, got, _ = Client(device).request("GET", "/api/status", headers={"Cookie": cookie})
    check(second is not None and set(second["state"]) == set(got), "the pushed state has the keys of the GET",
          str(sorted(got))[:80])
    # a page follows up to 3 paths on one socket
    live.send("/api/status /api/hotspot /api/nothing")
    paths = set()
    for _ in range(4):
        message = live.receive(3)
        if message is None:
            break
        paths.add(message.get("path"))
    check({"/api/status", "/api/hotspot"} <= paths, "two paths in one frame both come", str(sorted(map(str, paths))))
    live.send("")
    check(live.receive(4) is None, "an empty frame stops it")
    live.send("/api/nothing")
    check(live.receive(3) is None, "an unknown path sends nothing")
    check(live.receive(12, heartbeat=True) == {}, "a heartbeat {} comes at least every 10 s")
    time.sleep(38)
    live.send("/api/status")
    try:
        after = live.receive(3)
    except ConnectionError:
        after = None
    check(after is not None, "the live socket stays open after 40 s of silence (no idle close)")
    live.close()

    # a logout closes the live socket of its browser
    other = Client(device)
    other.request("POST", "/api/login", form={"username": user, "password": password, "remember": "0"})
    live = LiveSocket(device, other.cookie)
    live.send("/api/status")
    live.receive(3)
    other.request("POST", "/api/logout", form={})
    try:
        while live.receive(3) is not None:
            pass
        closed = False
    except ConnectionError:
        closed = True
    check(closed, "a logout closes its live socket")
    live.close()


def long_response_test(c: Client, device: str) -> None:
    """A long download runs on a task of its own (px/webserver/async.hpp): the API answers meanwhile.
    The answer shares the Wi-Fi with the download, so it is not instant; on the server's task it
    waited for the whole download. Passes when it came in a fifth of the download's time, at most 3 s."""
    def download(path: str) -> tuple[int, bytes, float]:
        started = time.time()
        conn = http.client.HTTPConnection(device, timeout=120)
        try:
            conn.request("GET", path, headers={"Cookie": c.cookie, "Accept-Encoding": "identity"})
            response = conn.getresponse()
            data = response.read()
            status = response.status
        except (http.client.HTTPException, OSError):
            # cut off by the device (no memory for both): a check that fails, not the end of the run
            status, data = 0, b""
        conn.close()
        return status, data, time.time() - started

    def answer_time() -> float:
        started = time.time()
        Client(device).request("GET", "/api/session", timeout=30)
        return time.time() - started

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        zips = [pool.submit(download, "/api/fs/files.zip") for _ in range(2)]
        time.sleep(0.5)
        waited = answer_time()
        results = [z.result() for z in zips]
    valid = 0
    for status, data, _ in results:
        try:
            with zipfile.ZipFile(io.BytesIO(data)) as z:
                valid += status == 200 and z.testzip() is None
        except zipfile.BadZipFile:
            pass
    took = min(r[2] for r in results)
    check(waited < min(3.0, took / 5) or took < 1.0, "the API answers while two files.zip are sent",
          f"{waited:.2f} s, the archives took {took:.1f} s")
    check(valid == 2, "two files.zip at once are both valid archives", f"{valid} of 2")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("device")
    parser.add_argument("--user", default="admin")
    parser.add_argument("--password")
    args = parser.parse_args()
    password = args.password or os.environ.get("PX_PASSWORD") or getpass.getpass(f"password for {args.user}: ")
    c = Client(args.device)

    # ------------------------------------------------------------------------ without a login
    status, page, _ = c.request("GET", "/")
    check(status == 200 and "<html" in str(page), "GET / answers a page")
    status, _, _ = c.request("GET", "/", headers={"Accept-Encoding": "identity"})
    check(status == 406, "GET / without gzip is 406 (the web image is gzip only)", str(status))
    status, _, headers = c.request("GET", "/")
    etag = headers.get("ETag", "")
    check(status == 200 and etag.startswith('"'), "GET / carries the image's ETag", etag)
    status, _, _ = c.request("GET", "/app.js", headers={"If-None-Match": etag})
    check(status == 304, "GET /app.js with the image's ETag is 304", str(status))
    status, page, _ = c.request("GET", "/recovery")
    check(status == 200 and "<html" in str(page), "GET /recovery answers the built-in page")
    status, session, _ = c.request("GET", "/api/session")
    check(status == 200 and session.get("setup") is False, "a login exists (not in setup mode)", str(session))
    for path in ["/api/status", "/api/wifi", "/api/update", "/api/hotspot", "/api/fs/files.zip"]:
        status, _, _ = c.request("GET", path)
        check(status == 401, f"GET {path} without login is 401", str(status))
    status, _, _ = c.request("POST", "/api/update/firmware", body=b"x" * 100,
                             headers={"Content-Type": "application/octet-stream"})
    check(status == 401, "an upload without login is 401", str(status))
    status, answer, _ = c.request("POST", "/api/setup/user", form={"username": "intruder", "password": "12345678"})
    check(status == 403, "a second login cannot be created", f"{status} {answer}")

    status, _, _ = c.request("POST", "/api/login", form={"username": args.user, "password": "wrong-password"})
    check(status == 401, "a wrong password is 401", str(status))
    status, _, _ = c.request("POST", "/api/login", form={"username": "", "password": ""})
    check(status in (400, 401), "an empty login is refused", str(status))
    status, _, _ = c.request("POST", "/api/login", body=b"username=" + b"a" * 8000,
                             headers={"Content-Type": "application/x-www-form-urlencoded"})
    check(status in (400, 413), "an 8 KB login body is refused", str(status))
    status, _, _ = c.request("POST", "/api/login", form={"username": args.user, "password": "%ZZ%"})
    check(status in (400, 401), "a malformed form value is refused", str(status))

    for path in ["/../../settings", "/%2e%2e/%2e%2e/etc", "/www/../../x", "/nothing.html", "/api/nothing"]:
        status, _, _ = c.request("GET", path)
        check(status == 404, f"GET {path} is 404", str(status))

    # ---------------------------------------------------------------------------- with a login
    status, _, headers = c.request("POST", "/api/login", form={"username": args.user, "password": password, "remember": "0"})
    cookie_header = headers.get("Set-Cookie", "")
    check(status == 200 and bool(c.cookie), "login", str(status))
    check("HttpOnly" in cookie_header and "SameSite=Strict" in cookie_header, "the session cookie is HttpOnly and SameSite=Strict",
          cookie_header)
    status, s, _ = c.request("GET", "/api/status")
    check(status == 200 and "device" in s and "wifi" in s and s["device"].get("built"), "GET /api/status",
          f"heap {s.get('device', {}).get('heap_internal_kb')} KB, built {s.get('device', {}).get('built')}")
    features = s.get("features") if status == 200 else None
    check(isinstance(features, list) and "leds" in features, "/api/status lists the features", str(features))
    heap_before = s["device"]["heap_internal_kb"] if status == 200 else 0
    status, w, _ = c.request("GET", "/api/wifi")
    check(status == 200 and "saved" in w, "GET /api/wifi", f"{len(w.get('saved', []))} saved")
    # `password_needed` is a flag, not a password
    check(all("password" not in n for n in w.get("saved", [])), "no password in the network list",
          str([sorted(n) for n in w.get("saved", [])]))
    status, u, _ = c.request("GET", "/api/update")
    check(status == 200 and u.get("app") in ("main", "recovery"), "GET /api/update", str(u.get("app")))
    status, answer, _ = c.request("POST", "/api/wifi/add", form={"password": "12345678"})
    check(status == 400, "a network without a name is refused", str(status))
    status, _, _ = c.request("POST", "/api/update/filesystem", body=b"",
                             headers={"Content-Type": "application/octet-stream"})
    check(status in (400, 411), "an empty image is refused", str(status))
    status, h, _ = c.request("GET", "/api/hotspot")
    check(status == 200 and {"on", "kept_on", "ssid", "password", "clients"} <= set(h), "GET /api/hotspot", str(status))
    for path, form in [("/api/wifi/remove", {"index": "7"}), ("/api/wifi/remove", {"index": "x"}),
                       ("/api/wifi/move", {"index": "0", "delta": "5"}), ("/api/wifi/policy", {"policy": "random"}),
                       ("/api/hotspot", {"on": "maybe"}), ("/api/factory-reset", {"confirm": "no"}),
                       ("/api/fs/format", {"confirm": "no"})]:
        status, _, _ = c.request("POST", path, form=form)
        check(status == 400, f"POST {path} {form} is refused", str(status))
    status, answer, _ = c.request("POST", "/api/update/url", form={"target": "firmware", "url": "ftp://x"})
    check(status in (400, 409), "a URL that is not http(s) is refused", f"{status} {answer}")

    # the scripts (a firmware with CONFIG_PX_SCRIPTS: 404 without them); read and refused only
    status, sc, _ = c.request("GET", "/api/scripts")
    if status == 404:
        print("      (no scripts in this firmware)")
    else:
        check(status == 200 and {"slots", "scripts", "run"} <= set(sc), "GET /api/scripts",
              f"{len(sc.get('scripts', []))} of {sc.get('slots')}")
        status, _, _ = c.request("GET", "/api/scripts/source?slug=Bad%20Name")
        check(status == 400, "a script's bad file name is refused", str(status))
        status, _, _ = c.request("POST", "/api/scripts/save?slug=solid", body=b'anim "x"',
                                 headers={"Content-Type": "text/plain"})
        check(status == 400, "a script cannot take a built-in animation's name", str(status))
        status, _, _ = c.request("POST", "/api/scripts/delete?slug=no-such-script")
        check(status == 404, "deleting a script that is not there is 404", str(status))

    # a new password over 64 characters is named as too long (the current one is wrong: nothing changes)
    status, answer, _ = c.request("POST", "/api/password", form={"current": "wrong-password", "password": "x" * 65})
    check(status == 400 and "8 to 64" in str(answer), "a new password of 65 characters is too long", f"{status} {answer}")

    conn = http.client.HTTPConnection(args.device, timeout=60)
    conn.request("GET", "/api/fs/files.zip", headers={"Cookie": c.cookie})
    response = conn.getresponse()
    archive = response.read()
    conn.close()
    try:
        with zipfile.ZipFile(io.BytesIO(archive)) as z:
            bad = z.testzip()
            names = z.namelist()
        check(response.status == 200 and bad is None, "GET /api/fs/files.zip is a valid archive", f"{len(names)} files")
    except zipfile.BadZipFile:
        check(False, "GET /api/fs/files.zip is a valid archive", f"{response.status}, {len(archive)} bytes")

    # ------------------------------------------------------------------------------------ load
    def fetch(i: int) -> tuple[int, float]:
        started = time.time()
        client = Client(args.device)
        client.cookie = c.cookie
        try:
            status, _, _ = client.request("GET", "/api/status" if i % 2 else "/", timeout=20)
        except OSError:
            status = 0
        return status, time.time() - started

    started = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        answers = list(pool.map(fetch, range(60)))
    good = sum(1 for status, _ in answers if status == 200)
    slowest = max(t for _, t in answers)
    check(good == len(answers), "60 requests from 8 connections at once",
          f"{good} ok in {time.time() - started:.1f} s, slowest {slowest:.2f} s")

    # A client that announces a body and stalls must not block the server for long.
    stall = socket.create_connection((args.device, 80), timeout=5)
    stall.sendall(b"POST /api/login HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                  b"Content-Length: 100\r\n\r\nusername=a")
    time.sleep(1)
    started = time.time()
    try:
        status, _, _ = c.request("GET", "/api/session", timeout=40)
    except OSError:
        status = 0
    waited = time.time() - started
    stall.close()
    check(status == 200 and waited < 30, "a stalled upload does not block other requests for long",
          f"{status} after {waited:.1f} s")
    idle_connections_test(args.device)
    long_response_test(c, args.device)
    idle_close_test(args.device)
    if s.get("live"):
        live_test(args.device, c.cookie, args.user, password)
        preview_test(args.device, c.cookie, features or [])
    else:
        print("  (no live socket, the recovery app: the live test skipped)")

    time.sleep(1)
    status, s, _ = c.request("GET", "/api/status")
    heap_after = s["device"]["heap_internal_kb"] if status == 200 else 0
    check(status == 200 and heap_after >= heap_before - 8, "free memory after the load",
          f"{heap_before} KB before, {heap_after} KB after")

    # ---------------------------------------------------------------------------------- logout
    cookie = c.cookie
    status, _, _ = c.request("POST", "/api/logout", form={})
    check(status == 200, "logout", str(status))
    c.cookie = cookie
    status, session, _ = c.request("GET", "/api/session")
    check(status == 200, "the old cookie after logout", f"logged_in {session.get('logged_in')} (the token stays valid until it expires)")

    failed = [name for ok, name in results if not ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
