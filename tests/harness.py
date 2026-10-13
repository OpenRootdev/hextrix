#!/usr/bin/env python3
"""Hextrix acceptance harness (pty + ANSI screen parser)."""
import fcntl
import os
import re
import struct
import subprocess
import sys
import termios
import time
import tty

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEXTRIX = os.path.join(ROOT, "hextrix")
RESULTS = []
HEX = re.compile(r"^[0-9A-Fa-f]{2}$")


def check(name, ok, detail=""):
    RESULTS.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + (f" — {detail}" if detail and not ok else (f" — {detail}" if detail and ok and False else "")), flush=True)
    if not ok:
        print(f"     detail: {detail}", flush=True)


# ---------------------------------------------------------------- screen

class Screen:
    def __init__(self, rows, cols):
        self.rows, self.cols = rows, cols
        self.ch = [[" "] * cols for _ in range(rows)]
        self.red = [[False] * cols for _ in range(rows)]
        self.sgr_codes = set()
        self.cx = self.cy = 0
        self.cur_red = False

    def resize(self, rows, cols):
        self.__init__(rows, cols)

    def feed(self, data):
        i, n = 0, len(data)
        while i < n:
            b = data[i]
            if b == 0x1b:
                if i + 1 < n and data[i + 1] in (ord("("), ord(")"),
                                                  ord("*"), ord("+")):
                    i += 3          # ESC ( X charset designation
                    continue
                if i + 1 < n and data[i + 1] != ord("["):
                    i += 2          # ESC = , ESC > , ESC M , ...
                    continue
                if i + 1 < n and data[i + 1] == ord("["):
                    j = i + 2
                    while j < n and not (0x40 <= data[j] <= 0x7e):
                        j += 1
                    if j < n:
                        self.csi(data[i + 2:j].decode("latin1"), chr(data[j]))
                        i = j + 1
                        continue
                i += 1  # ESC + unknown: skip
            elif b == 0x0a:
                self.cy = min(self.cy + 1, self.rows - 1)
            elif b == 0x0d:
                self.cx = 0
            elif b == 0x08:
                self.cx = max(0, self.cx - 1)
            elif b == 0x09:
                self.cx = min(self.cols - 1, (self.cx // 8 + 1) * 8)
            elif 0x20 <= b < 0x7f or b >= 0x80:
                if 0 <= self.cy < self.rows and 0 <= self.cx < self.cols:
                    self.ch[self.cy][self.cx] = chr(b)
                    self.red[self.cy][self.cx] = self.cur_red
                self.cx += 1
                if self.cx >= self.cols:
                    self.cx = 0
                    self.cy = min(self.cy + 1, self.rows - 1)
            i += 1

    def csi(self, body, final):
        parts = body.split(";") if body else []
        nums = [int(p) if p.isdigit() else 0 for p in parts]
        if final == "H" or final == "f":
            r = nums[0] if len(nums) > 0 else 1
            c = nums[1] if len(nums) > 1 else 1
            self.cy, self.cx = min(max(r - 1, 0), self.rows - 1), \
                               min(max(c - 1, 0), self.cols - 1)
        elif final == "A":
            self.cy = max(0, self.cy - (nums[0] if nums and nums[0] else 1))
        elif final == "B":
            self.cy = min(self.rows - 1, self.cy + (nums[0] if nums and nums[0] else 1))
        elif final == "C":
            self.cx = min(self.cols - 1, self.cx + (nums[0] if nums and nums[0] else 1))
        elif final == "D":
            self.cx = max(0, self.cx - (nums[0] if nums and nums[0] else 1))
        elif final == "G" or final == "`":
            col = nums[0] if nums and nums[0] else 1
            self.cx = min(max(col - 1, 0), self.cols - 1)
        elif final == "d":
            row = nums[0] if nums and nums[0] else 1
            self.cy = min(max(row - 1, 0), self.rows - 1)
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 2:
                self.ch = [[" "] * self.cols for _ in range(self.rows)]
                self.red = [[False] * self.cols for _ in range(self.rows)]
        elif final == "K":
            for x in range(self.cx, self.cols):
                self.ch[self.cy][x] = " "
                self.red[self.cy][x] = False
        elif final == "m":
            self.sgr(body)

    def sgr(self, body):
        codes = [int(x) if x else 0 for x in body.split(";")] if body else [0]
        i = 0
        while i < len(codes):
            c = codes[i]
            self.sgr_codes.add(c)
            if c == 0:
                self.cur_red = False
            elif c in (31, 91):
                self.cur_red = True
            elif c == 39 or 30 <= c <= 37 or 90 <= c <= 97:
                self.cur_red = False
            elif c == 38 and i + 2 < len(codes):
                if codes[i + 1] == 5 and codes[i + 2] in (1, 31):
                    self.cur_red = True
                else:
                    self.cur_red = False
                i += 2
            i += 1

    # ---- inspection ------------------------------------------------
    def text(self):
        return ["".join(row) for row in self.ch]

    def red_count(self):
        return sum(sum(1 for v in row if v) for row in self.red)

    def red_mask(self):
        return [row[:] for row in self.red]


# ---------------------------------------------------------------- app

class App:
    def __init__(self, rows=40, cols=120, args=(), log=None):
        self.screen = Screen(rows, cols)
        self.master, slave = os.openpty()
        for fd in (self.master,):
            fl = fcntl.fcntl(fd, fcntl.F_GETFL)
            fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        env = dict(os.environ, TERM="xterm-256color", ESCDELAY="25")
        if log:
            env["HEXTRIX_LOG"] = log
        self.log = log

        def child_setup():
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

        self.proc = subprocess.Popen(
            [HEXTRIX] + list(args), stdin=slave, stdout=slave, stderr=slave,
            env=env, preexec_fn=child_setup, close_fds=True)
        os.close(slave)

    def pump(self, seconds):
        end = time.time() + seconds
        buf = b""
        while time.time() < end:
            try:
                chunk = os.read(self.master, 65536)
                if chunk:
                    buf += chunk
            except (BlockingIOError, OSError):
                pass
            time.sleep(0.01)
        if buf:
            self.tail = buf
            self.screen.feed(buf)

    def send(self, s):
        os.write(self.master, s.encode())

    def resize(self, rows, cols):
        winsz = struct.pack("HHHH", rows, cols, 0, 0)
        fcntl.ioctl(self.master, termios.TIOCSWINSZ, winsz)
        self.screen.resize(rows, cols)

    def alive(self):
        return self.proc.poll() is None

    def wait_exit(self, timeout):
        try:
            return self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            return None

    def read_log(self):
        if not self.log:
            return ""
        try:
            with open(self.log) as f:
                return f.read()
        except OSError:
            return ""

    def close(self):
        try:
            os.close(self.master)
        except OSError:
            pass


# ---------------------------------------------------------------- helpers

def avg_red(app, seconds):
    vals = []
    end = time.time() + seconds
    while time.time() < end:
        app.pump(0.15)
        vals.append(app.screen.red_count())
    return sum(vals) / max(1, len(vals))


def token_map(screen):
    m = {}
    for y in range(screen.rows):
        for x in range(0, screen.cols - 1, 3):
            m[(y, x)] = screen.ch[y][x] + screen.ch[y][x + 1]
    return m


def changed_tokens(a, b):
    return sum(1 for k in a if a[k] != b[k])


# ---------------------------------------------------------------- tests

def test_basic():
    app = App(rows=40, cols=120, log="/tmp/hx_basic.log")
    app.pump(2.5)
    s = app.screen
    total = s.rows * s.cols

    filled = sum(1 for y in range(s.rows) for x in range(s.cols - 1)
                 if s.ch[y][x] != " ")
    check("grid fills terminal", filled > total * 0.60, f"fill={filled/total:.2f}")

    toks = [(y, x, s.ch[y][x] + s.ch[y][x + 1])
            for y in range(s.rows) for x in range(0, s.cols - 1, 3)]
    bad = [t for t in toks if not HEX.match(t[2])]
    check("hex tokens everywhere", not bad and len(toks) > 400,
          f"{len(bad)} bad of {len(toks)}")
    check("single-space separators",
          all(s.ch[y][x + 2] == " " for y, x, _ in toks if x + 2 < s.cols))

    allowed = (0, 1, 2, 22, 31, 39, 49, 24, 27, 38, 90)
    bad_sgr = sorted(c for c in s.sgr_codes if c not in allowed)
    check("only expected SGR codes", not bad_sgr, f"sgr={sorted(s.sgr_codes)}")
    check("body text uses deep gray (SGR 90)", 90 in s.sgr_codes,
          f"sgr={sorted(s.sgr_codes)}")
    check("body text is dim", 2 in s.sgr_codes, f"sgr={sorted(s.sgr_codes)}")
    check("red is bold", 1 in s.sgr_codes and 31 in s.sgr_codes,
          f"sgr={sorted(s.sgr_codes)}")

    red0 = s.red_count()
    check("idle red visible", total * 0.02 < red0 < total * 0.60,
          f"red={red0}/{total}")
    m0 = s.red_mask()
    app.pump(1.5)
    m1 = app.screen.red_mask()
    diff = sum(1 for a, b in zip(m0, m1) for u, v in zip(a, b) if u != v)
    check("idle red moves", diff > 80, f"changed={diff} cells")

    fps = [float(l.split("fps=")[1].split()[0])
           for l in app.read_log().splitlines() if "fps=" in l]
    check("frame rate 25-65", bool(fps) and all(25 <= f <= 65 for f in fps),
          f"fps={fps}")
    check("dim logged", "dim: color 8 + A_DIM" in app.read_log())

    app.send("q")
    app.pump(0.4)
    check("q exits 0", app.wait_exit(5) == 0)
    app.pump(0.2)
    tail = app.tail.decode("utf-8", "replace")
    check("alternate screen restored", "\x1b[?1049l" in tail)
    check("cursor restored", "\x1b[?25h" in tail)
    app.close()


def test_keys():
    # no audio: keeps intensity measurements deterministic
    app = App(rows=40, cols=120, args=["-n"], log="/tmp/hx_keys.log")
    app.pump(2.0)

    app.send(" ")
    app.pump(0.4)
    m0 = app.screen.red_mask()
    t0 = "".join(app.screen.text())
    app.pump(1.0)
    check("space pauses everything",
          m0 == app.screen.red_mask() and t0 == "".join(app.screen.text()))
    app.send(" ")
    app.pump(0.5)

    app.send("h")
    app.pump(0.5)
    txt = "".join(app.screen.text())
    check("h shows help", "pause / resume" in txt)
    check("help documents visualizer", "visualizer" in txt)
    app.send("h")
    app.pump(0.6)
    app.send("h")
    app.pump(0.4)
    check("h reopens help", "pause / resume" in "".join(app.screen.text()))
    app.send("\x1b")
    app.pump(0.5)
    check("esc closes help",
          "pause / resume" not in "".join(app.screen.text()))
    app.send("?")
    app.pump(0.4)
    check("? opens help", "pause / resume" in "".join(app.screen.text()))
    app.send("?")
    app.pump(0.5)
    check("? closes help",
          "pause / resume" not in "".join(app.screen.text()))

    # bare Esc with nothing open must not stall the loop (escdelay fix)
    m0 = app.screen.red_mask()
    app.send("\x1b")
    app.pump(0.25)
    check("esc alone does not stall the app",
          app.screen.red_mask() != m0)

    # time-averaged: the crest rolls, so single frames swing up to ~40%
    r0 = avg_red(app, 1.6)
    for _ in range(6):
        app.send("+")
    r_hi = avg_red(app, 1.6)
    for _ in range(12):
        app.send("-")
    app.pump(3.0)                     # let the boosted dye decay away
    r_lo = avg_red(app, 1.6)
    check("+ increases red, - decreases",
          r_hi > r0 > r_lo and r_hi > r_lo * 1.4,
          f"{r0:.0f} -> {r_hi:.0f} -> {r_lo:.0f}")
    for _ in range(6):
        app.send("+")

    # token rate: fast -> freeze -> resume must work
    for _ in range(20):
        app.send("\x1bOA")
    app.pump(0.1)
    a = token_map(app.screen)
    app.pump(1.5)
    check("up arrow speeds up token changes",
          changed_tokens(a, token_map(app.screen)) > 400)
    for _ in range(30):
        app.send("\x1bOB")
    app.pump(0.1)
    a = token_map(app.screen)
    app.pump(1.5)
    check("down arrow freezes token changes",
          changed_tokens(a, token_map(app.screen)) == 0)
    for _ in range(3):
        app.send("\x1bOA")
    app.pump(0.1)
    a = token_map(app.screen)
    app.pump(1.5)
    check("up arrow resumes after freeze",
          changed_tokens(a, token_map(app.screen)) > 0)

    # v cycles visualizers and back to liquid (default off)
    seen = []
    for name in ("wave", "rain", "liquid"):
        app.send("v")
        app.pump(0.25)
        flash = "".join(app.screen.text())
        check(f"flash shows '{name}'", f"[ {name} ]" in flash,
              [l for l in app.screen.text() if name in l][:2])
        app.pump(0.2)
        seen.append(name)
    app.pump(1.5)
    log = app.read_log()
    logged = [s for s in seen if f"style: {s}" in log]
    check("v cycles visualizer styles (log)", logged == seen, f"logged={logged}")
    check("no removed styles", all(x not in log for x in ("swell", "pulse")),
          [l for l in log.splitlines() if "style" in l][:5])
    check("flash disappears", all("[ " not in l for l in app.screen.text()))

    app.send("\x03")
    check("Ctrl+C exits 0", app.wait_exit(5) == 0)
    app.close()


def test_styles():
    app = App(rows=30, cols=80, log="/tmp/hx_styles.log")
    app.pump(1.5)
    stats = []
    for style in ("liquid", "wave", "rain"):
        if style != "liquid":
            app.send("v")
            app.pump(0.3)
        app.pump(6.0)
        s = app.screen
        red = s.red_count()
        total = s.rows * s.cols
        m0 = s.red_mask()
        app.pump(1.5)
        m1 = app.screen.red_mask()
        churn = sum(1 for a, b in zip(m0, m1) for u, v in zip(a, b) if u != v)
        stats.append((style, red / total, churn))
        print(f"     {style:6s} red={red/total:.2f} churn={churn}")
        check(f"{style} animates", churn > 50, f"churn={churn}")
        check(f"{style} not flooded", red / total < 0.70,
              f"red={red/total:.2f}")
        if style == "wave":
            bottom = sum(1 for y in range(s.rows // 2, s.rows)
                         for x in range(s.cols) if s.red[y][x])
            share = bottom / max(1, red)
            check("wave sits at the bottom", share > 0.6,
                  f"bottom-half share={share:.2f}")
    # styles must differ from each other
    fracs = [f for _, f, _ in stats]
    check("styles differ", max(fracs) - min(fracs) > 0.03, f"fracs={fracs}")
    app.send("q")
    check("styles: clean exit", app.wait_exit(5) == 0)
    app.close()


def test_colors():
    app = App(rows=30, cols=100, log="/tmp/hx_colors.log")
    app.pump(2.0)
    s0 = set(app.screen.sgr_codes)
    check("default accent is red", 31 in s0, f"sgr={sorted(s0)}")

    names = {2: ("green", 32), 5: ("magenta", 35), 9: ("bright magenta", 95),
             1: ("red", 31)}
    for key, (name, sgr) in names.items():
        app.send(str(key))
        app.pump(0.25)
        txt = "".join(app.screen.text())
        check(f"key {key} flashes '{name}'", f"[ {name} ]" in txt,
              [l for l in app.screen.text() if name in l][:2])
        app.pump(1.6)          # flash expires, full repaint in new color
        check(f"key {key} renders SGR {sgr}", sgr in app.screen.sgr_codes,
              f"sgr={sorted(app.screen.sgr_codes)}")
        if key == 2:   # first flash: plate must add a background
            check("hint sits on a dim plate", 100 in app.screen.sgr_codes,
                  f"sgr={sorted(app.screen.sgr_codes)}")

    log = app.read_log()
    logged = [n for n in ("green", "magenta", "bright magenta", "red")
              if f"accent: {n}" in log]
    check("accent changes logged", len(logged) == 4, f"logged={logged}")
    app.send("q")
    app.pump(0.4)
    check("colors: clean exit", app.wait_exit(5) == 0)
    app.close()


def red_churn(app, seconds):
    m0 = app.screen.red_mask()
    app.pump(seconds)
    m1 = app.screen.red_mask()
    return sum(1 for a, b in zip(m0, m1)
               for u, v in zip(a, b) if u != v)


def test_speed():
    app = App(rows=30, cols=80, args=["-n"], log="/tmp/hx_speed.log")
    app.pump(2.0)

    for _ in range(4):
        app.send("\x1bOC")             # right arrow, clamps at the top
    app.pump(0.3)
    txt = "".join(app.screen.text())
    check("right arrow flashes speed", "[ speed 4x ]" in txt,
          [l for l in app.screen.text() if "speed" in l][:2])
    check("speed hint sits on a dim plate", 100 in app.screen.sgr_codes,
          f"sgr={sorted(app.screen.sgr_codes)}")
    app.pump(1.5)
    check("max speed logged", "speed: 4.00" in app.read_log(),
          [l for l in app.read_log().splitlines() if "speed" in l])

    fast = red_churn(app, 1.4)

    for _ in range(20):
        app.send("\x1bOD")             # left arrow, clamps at the floor
    app.pump(0.3)
    txt = "".join(app.screen.text())
    check("left arrow flashes slow speed", "[ speed 0.15x ]" in txt,
          [l for l in app.screen.text() if "speed" in l][:2])
    app.pump(1.5)                       # flash expires
    check("min speed logged", "speed: 0.15" in app.read_log(),
          [l for l in app.read_log().splitlines() if "speed" in l])

    slow = red_churn(app, 1.4)
    check("right speeds up fluid, left slows",
          fast > slow * 1.5, f"fast={fast} slow={slow}")

    app.send("q")
    check("speed: clean exit", app.wait_exit(5) == 0)
    app.close()


def test_resize():
    app = App(rows=40, cols=120, log="/tmp/hx_resize.log")
    app.pump(1.5)
    for (r, c) in ((30, 90), (45, 100), (3, 8)):
        app.resize(r, c)
        app.pump(1.0)
        check(f"resize {c}x{r}", app.alive() and
              app.screen.rows == r and app.screen.cols == c)
    app.resize(40, 120)
    app.pump(1.0)
    a = token_map(app.screen)
    app.pump(1.5)
    check("animates after resize", changed_tokens(a, token_map(app.screen)) > 100
          or app.screen.red_count() > 0)
    app.send("q")
    check("resize: clean exit", app.wait_exit(5) == 0)
    app.close()


def main():
    if not os.path.exists(HEXTRIX):
        print("build first: make")
        return 1
    which = sys.argv[1] if len(sys.argv) > 1 else "all"
    tests = {
        "basic": test_basic,
        "keys": test_keys,
        "styles": test_styles,
        "colors": test_colors,
        "speed": test_speed,
        "resize": test_resize,
    }
    if which == "all":
        for t in tests.values():
            t()
    elif which in tests:
        tests[which]()
    else:
        print("usage: harness.py [all|basic|keys|styles|colors|speed|resize]")
        return 2
    ok = sum(1 for _, p in RESULTS if p)
    print(f"\n{ok}/{len(RESULTS)} passed")
    return 0 if ok == len(RESULTS) else 1


if __name__ == "__main__":
    sys.exit(main())
