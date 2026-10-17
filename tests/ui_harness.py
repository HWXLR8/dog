#!/usr/bin/env python3
"""A minimal ANSI-aware terminal emulator for driving and asserting on the dog UI.

Reconstructs the on-screen grid from CSI/SGR output and tracks the cursor
position and visibility. Tests assert on the grid and cursor state.

Usage:
    import ui_harness as H
    H.SCRIPT = [{"name": "ask_user", "tool": json.dumps(ask)}, {"text": "Done."}]
    d = H.start(dogbin, port=8123)      # starts a mock OpenAI server + dog in a pty
    d.pump(3.0)
    d.send("go\r")                        # raw keystrokes; submit is CR, not LF
    d.pump(2.0)
    s = d.scr                              # the reconstructed Screen
    d.kill()

Screen exposes:
    .rows .cols            fixed grid dimensions (default 40x160)
    .cr .cc                0-based cursor row / col
    .cursor_visible        whether the terminal cursor is shown
    .lines()               list of 40 strings, rstripped
    .row(r)                a single rstripped row
    .nonempty()            only rows with content
"""
import os, pty, time, select, signal, json, fcntl, struct, termios, socket
from http.server import BaseHTTPRequestHandler, HTTPServer


class Screen:
    def __init__(self, rows=40, cols=160):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.cr, self.cc = 0, 0
        self.cursor_visible = True
        self._pend = b""

    # -- text output --
    def _put(self, ch):
        w = self._width(ch)
        if self.cc >= self.cols:
            self._newline(); return
        if self.cr < self.rows:
            self.grid[self.cr][self.cc] = ch
        self.cc += w

    @staticmethod
    def _width(ch):
        if not ch: return 1
        cp = ord(ch[0]) if isinstance(ch, str) else ch
        if cp >= 0x1100 and (cp in (0x2000, 0x3000) or 0xFF00 <= cp <= 0xFF60 or
                             0x2E80 <= cp <= 0x303E or 0x3041 <= cp <= 0x33FF or
                             0x3400 <= cp <= 0x4DBF or 0x4E00 <= cp <= 0x9FFF or
                             0xA000 <= cp <= 0xA4CF or 0xAC00 <= cp <= 0xD7A3 or
                             0xF900 <= cp <= 0xFAFF or 0xFE30 <= cp <= 0xFE4F or
                             0xFFE0 <= cp <= 0xFFE6):
            return 2
        return 1

    def _decode_cp(self, data, i, n):
        b = data[i]
        if b < 0x80:
            return chr(b), i + 1
        if (b >> 5) == 0b110:
            return chr((b & 0x1F) << 6 | (data[i+1] & 0x3F)), i + 2
        if (b >> 4) == 0b1110:
            return chr((b & 0x0F) << 12 | (data[i+1] & 0x3F) << 6 | (data[i+2] & 0x3F)), i + 3
        if (b >> 3) == 0b11110:
            cp = (b & 0x07) << 18 | (data[i+1] & 0x3F) << 12 | (data[i+2] & 0x3F) << 6 | (data[i+3] & 0x3F)
            return chr(cp), i + 4
        return "", i + 1

    def _newline(self):
        self.cr = min(self.rows - 1, self.cr + 1)
        self.cc = 0

    def feed(self, data: bytes):
        data = self._pend + data
        self._pend = b""
        i, n = 0, len(data)
        # Hold a trailing partial UTF-8 rune so a multibyte char split across reads
        # never underflows the decoder.
        k = min(4, n)
        for back in range(k):
            b = data[n - 1 - back]
            if b & 0xC0 == 0x80:
                continue
            need = 1
            if b >= 0xF0: need = 4
            elif b >= 0xE0: need = 3
            elif b >= 0xC0: need = 2
            if back + 1 < need:
                self._pend = data[n - 1 - back:]
                n -= back + 1
            break
        while i < n:
            b = data[i]
            if b == 0x1b:
                i = self._escape(data, i, n)
            elif b == 0x0a:
                self._newline(); i += 1
            elif b == 0x0d:
                self.cc = 0; i += 1
            elif b == 0x08:
                if self.cc > 0: self.cc -= 1
                i += 1
            elif b < 0x20:
                i += 1
            else:
                ch, i2 = self._decode_cp(data, i, n)
                if ch: self._put(ch)
                i = i2

    def _escape(self, data, i, n):
        if i + 1 >= n:
            return i + 1
        b = data[i + 1]
        if b == 0x5b:  # '[' CSI
            return self._csi(data, i + 2, n)
        if b == 0x5d:  # ']' OSC: skip to BEL or ST
            j = i + 2
            while j < n:
                if data[j] == 0x07: break
                if data[j] == 0x1c and j + 1 < n and data[j+1] == 0x9d: j += 1; break
                j += 1
            return j + 1
        return i + 2  # other 2-byte sequences

    def _csi(self, data, i, n):
        priv = False
        params = []
        cur = ""
        while i < n:
            c = data[i]
            if c == 0x3b:  # ';'
                params.append(cur); cur = ""; i += 1
            elif 0x30 <= c <= 0x3f:  # param bytes
                if c == 0x3f or c in (0x3e, 0x3d, 0x21) and priv:  # '?' '>' '=' '!'
                    if c == 0x3f: priv = True
                cur += bytes([c]).decode()
                i += 1
            elif 0x40 <= c <= 0x7e:  # final byte
                if cur: params.append(cur)
                self._dispatch(priv, params, chr(c))
                return i + 1
            else:
                i += 1
        return i

    def _dispatch(self, priv, params, final):
        def pv(idx):
            try:
                return int(params[idx]) if idx < len(params) and params[idx].isdigit() else 0
            except Exception:
                return 0
        num = pv(0)
        if priv:
            if final == "h" and num == 25: self.cursor_visible = True
            elif final == "l" and num == 25: self.cursor_visible = False
        elif final == "H" or final == "f":
            r = pv(0); c = pv(1)
            # CUP: no params means row 1, col 1 (home). Always set both.
            self.cr = max(0, min(self.rows - 1, (r or 1) - 1))
            self.cc = max(0, min(self.cols - 1, (c or 1) - 1))
        elif final == "d":
            self.cr = max(0, min(self.rows - 1, (num or 1) - 1)); self.cc = 0
        elif final == "G":
            self.cc = max(0, min(self.cols - 1, (num or 1) - 1))
        elif final == "A":
            self.cr = max(0, self.cr - (num or 1))
        elif final == "B":
            self.cr = min(self.rows - 1, self.cr + (num or 1))
        elif final == "K":
            for cc in range(self.cc, self.cols): self.grid[self.cr][cc] = " "
        elif final == "J":
            for rr in range(self.cr, self.rows):
                for cc in range(self.cols): self.grid[rr][cc] = " "
        elif final == "m":
            pass  # SGR styling ignored
        # others ignored

    def row(self, r):
        return "".join(self.grid[r]).rstrip()

    def lines(self):
        return ["".join(self.grid[r]).rstrip() for r in range(self.rows)]

    def nonempty(self):
        return [ln for ln in self.lines() if ln.strip()]


class Dog:
    def __init__(self, dogbin, cfgpath, cwd, rows=40, cols=160):
        self._dogbin = dogbin
        self._pid, self._fd = pty.fork()
        if self._pid == 0:
            os.chdir(cwd)
            os.environ["TERM"] = "xterm-256color"
            os.execv(dogbin, [dogbin, "-c", cfgpath])
        else:
            # Set a fixed window size so the layout is deterministic in tests.
            try:
                fcntl.ioctl(self._fd, termios.TIOCSWINSZ,
                            struct.pack("HHHH", rows, cols, 0, 0))
            except Exception:
                pass
        self.scr = Screen(rows, cols)

    def pump(self, dur):
        end = time.time() + dur
        while True:
            r, _, _ = select.select([self._fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self._fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                self.scr.feed(data)
            if time.time() >= end:
                break
        self._drain(0.0)

    def _drain(self, settle):
        while True:
            r, _, _ = select.select([self._fd], [], [], settle)
            if not r:
                break
            try:
                data = os.read(self._fd, 65536)
            except OSError:
                break
            if not data:
                break
            self.scr.feed(data)

    def send(self, s, delay=0.0):
        if isinstance(s, str): s = s.encode()
        for b in s:
            os.write(self._fd, bytes([b]))
            if delay: time.sleep(delay)

    def kill(self):
        try: os.kill(self._pid, signal.SIGKILL)
        except Exception: pass
        try: os.waitpid(self._pid, 0)
        except Exception: pass


SCRIPT = []


def mock_server(port):
    class H(BaseHTTPRequestHandler):
        state = {"n": 0}
        def log_message(self, *a): pass
        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            self.rfile.read(n)
            H.state["n"] += 1
            k = H.state["n"]
            idx = min(k - 1, len(SCRIPT) - 1)
            item = SCRIPT[idx]
            def sse(payloads):
                return b"".join(b"data: " + json.dumps(p).encode() + b"\n\n" for p in payloads) + b"data: [DONE]\n\n"
            if "tool" in item:
                data = sse([
                    {"choices": [{"delta": {"tool_calls": [{"index": 0, "id": "c1",
                        "function": {"name": item["name"], "arguments": item["tool"]}}]}}]},
                    {"choices": [{"delta": {}}], "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}}])
            else:
                data = sse([
                    {"choices": [{"delta": {"content": item["text"]}}]},
                    {"choices": [{"delta": {}}], "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}}])
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(data)
    import threading
    srv = HTTPServer(("127.0.0.1", port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def _free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def start(dogbin, port=None):
    """Start a mock OpenAI server + dog in a pty. Returns a Dog handle."""
    global SCRIPT
    if port is None:
        port = _free_port()
    mock_server(port)
    cfgpath = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_cfg.json")
    open(cfgpath, "w").write(json.dumps({"model": {"base_url": f"http://127.0.0.1:{port}/v1", "api_key": "k", "model": "m"}}))
    d = Dog(dogbin, cfgpath, os.path.dirname(os.path.abspath(dogbin)) or ".")
    return d


def report(name, checks):
    print(f"\n=== {name} ===")
    allok = True
    for cn, ok in checks:
        print(f"  [{'PASS' if ok else 'FAIL'}] {cn}")
        allok = allok and ok
    print("  ->", "PASS" if allok else "FAIL")
    return allok
