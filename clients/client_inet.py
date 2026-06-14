#!/usr/bin/env python3
"""
client_inet.py — curses UI for the code-execution server.

Non-file-transfer commands are dispatched asynchronously: a dedicated
recv thread reads RespHeaders + payloads off the socket and puts results
in a thread-safe Queue.  The main curses loop polls that queue every 50ms
(mnu.timeout) so the UI never freezes while waiting for the server.

Wire protocol mirrors protocol.h:
  ReqHeader  '!BQ64sII'  (81 bytes)  language | code_size | filename[64] | time_limit_s | mem_limit_mb
  RespHeader '!64sQ'     (72 bytes)  filename[64] | file_size

Text commands use language=0xFF with code_size=0.
"""
import curses
import locale
import logging
import os
import queue
import socket
import struct
import sys
import threading
import time

# ── Client logging ────────────────────────────────────────────────────────────
# ncurses owns the terminal; write logs to session_client.log only.
_log = logging.getLogger("client_inet")
_log.setLevel(logging.DEBUG)
_log_handler = logging.FileHandler("session_client.log", encoding="utf-8")
_log_handler.setFormatter(logging.Formatter(
    "[%(asctime)s] [%(levelname).3s] %(message)s",
    datefmt="%H:%M:%S",
))
_log.addHandler(_log_handler)
_log.propagate = False

HOST = '127.0.0.1'
PORT = 8080

REQ_FMT = '!BQ64sII'
RESP_FMT = '!64sQ'
REQ_SZ = struct.calcsize(REQ_FMT)   # 81
RESP_SZ = struct.calcsize(RESP_FMT)  # 72

MENU_W = 22

CP_HDR, CP_STS, CP_ERR, CP_SEL = 1, 2, 3, 4

MENU_ITEMS = [
    "Run Code",
    "Ping Server",
    "Languages",
    "Server Status",
    "Queue Depth",
    "Who Am I",
    "Help",
    "Quit",
]

CMD_MAP = {1: "PING", 2: "LANGS", 3: "STATUS",
           4: "QUEUE", 5: "WHOAMI", 6: "HELP"}
TITLE_MAP = {1: "PING", 2: "SUPPORTED LANGUAGES", 3: "SERVER STATUS",
             4: "QUEUE DEPTH", 5: "WHO AM I", 6: "HELP"}
EXT_LANG = {
    '.c': 1, '.py': 2, '.cpp': 3, '.java': 4,
    '.sh': 5, '.js': 6, '.rb': 7, '.go': 8, '.rs': 9,
    '.php': 10, '.pl': 11, '.ts': 12, '.hs': 13, '.lua': 14,
    '.r': 15, '.swift': 16, '.kt': 17, '.f90': 18,
}

# ── network helpers ──────────────────────────────────────────────────────────


def recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("server closed the connection")
        buf.extend(chunk)
    return bytes(buf)


def recv_file_to_disk(sock, path, fsize):
    with open(path, 'wb') as f:
        rem = fsize
        while rem > 0:
            chunk = sock.recv(min(65536, rem))
            if not chunk:
                raise ConnectionError("connection lost while receiving file")
            f.write(chunk)
            rem -= len(chunk)


def make_output_name(src_path):
    stem = os.path.splitext(os.path.basename(src_path))[0]
    return f"output_{stem}.txt"


def detect_language(path):
    return EXT_LANG.get(os.path.splitext(path)[1].lower(), 0)


def lang_name(lid):
    return {1: 'C', 2: 'Python', 3: 'C++', 4: 'Java'}.get(lid, 'Unknown')

# ── recv thread ──────────────────────────────────────────────────────────────


def recv_thread(sock, resp_queue, hint):
    """
    Daemon thread: continuously reads RespHeader + payload from the socket.
    Results are put in resp_queue for the main thread to consume.

    hint is {'lock': threading.Lock, 'is_file': bool, 'save_path': str}.
    The main thread sets hint before each send; the response cannot arrive
    before the send, so the hint is always set when this thread reads it.
    """
    try:
        while True:
            raw = recv_exact(sock, RESP_SZ)
            _, fsize = struct.unpack(RESP_FMT, raw)

            with hint['lock']:
                is_file = hint['is_file']
                save_path = hint['save_path']

            if is_file and save_path:
                recv_file_to_disk(sock, save_path, fsize)
                resp_queue.put(
                    {'is_file': True, 'save_path': save_path, 'bytes': fsize})
            else:
                text = recv_exact(sock, fsize).decode(
                    errors='replace') if fsize else ''
                resp_queue.put(
                    {'is_file': False, 'text': text, 'bytes': fsize})

    except (ConnectionError, OSError):
        resp_queue.put({'error': True})

# ── curses helpers ───────────────────────────────────────────────────────────


def _safe_addstr(win, y, x, text, attr=0):
    try:
        win.addstr(y, x, text, attr) if attr else win.addstr(y, x, text)
    except curses.error:
        pass


def init_colors():
    curses.init_pair(CP_HDR, curses.COLOR_WHITE, curses.COLOR_BLUE)
    curses.init_pair(CP_STS, curses.COLOR_BLACK, curses.COLOR_WHITE)
    curses.init_pair(CP_ERR, curses.COLOR_WHITE, curses.COLOR_RED)
    curses.init_pair(CP_SEL, curses.COLOR_BLACK, curses.COLOR_CYAN)


def draw_header(win):
    _, C = win.getmaxyx()
    title = "CODE EXECUTION CLIENT"
    win.bkgd(' ', curses.color_pair(CP_HDR) | curses.A_BOLD)
    win.erase()
    _safe_addstr(win, 0, max(0, (C - len(title)) // 2), title)
    win.noutrefresh()


def set_status(win, msg, err=False):
    _, C = win.getmaxyx()
    win.bkgd(' ', curses.color_pair(CP_ERR if err else CP_STS))
    win.erase()
    _safe_addstr(win, 0, 1, msg[:C - 2])
    win.noutrefresh()


def draw_menu(win, sel):
    R, C = win.getmaxyx()
    win.erase()
    win.box()
    _safe_addstr(win, 1, 2, "MENU", curses.A_BOLD)
    pad = max(1, C - 3)
    for i, label in enumerate(MENU_ITEMS):
        row = 3 + i
        if row >= R - 1:
            break
        text = f" {label[:pad - 1]:<{pad - 1}}"
        attr = (curses.color_pair(CP_SEL) | curses.A_BOLD) if i == sel else 0
        _safe_addstr(win, row, 1, text, attr)
    win.noutrefresh()


def show_content(win, title, text):
    R, C = win.getmaxyx()
    win.erase()
    win.box()
    _safe_addstr(win, 0, 2, f" {title} ", curses.A_BOLD)
    max_r, max_c = R - 2, C - 4
    r = 1
    for line in text.split('\n'):
        if r > max_r:
            break
        _safe_addstr(win, r, 2, line[:max_c])
        r += 1
    win.noutrefresh()


def show_info_box(win, msg):
    R, C = win.getmaxyx()
    mlen = len(msg)
    bw = mlen + 6
    if bw < 30:
        bw = 30
    if bw > C - 4:
        bw = C - 4
    bh = 5
    by = (R - bh) // 2
    bx = (C - bw) // 2
    win.erase()
    win.box()
    try:
        win.addch(by, bx, curses.ACS_ULCORNER)
        win.hline(by, bx + 1, curses.ACS_HLINE, bw - 2)
        win.addch(by, bx + bw - 1, curses.ACS_URCORNER)
        for y in range(by + 1, by + bh - 1):
            win.addch(y, bx, curses.ACS_VLINE)
            win.hline(y, bx + 1, ord(' '), bw - 2)
            win.addch(y, bx + bw - 1, curses.ACS_VLINE)
        win.addch(by + bh - 1, bx, curses.ACS_LLCORNER)
        win.hline(by + bh - 1, bx + 1, curses.ACS_HLINE, bw - 2)
        win.addch(by + bh - 1, bx + bw - 1, curses.ACS_LRCORNER)
        ttl = ' WORKING '
        win.addstr(by, bx + (bw - len(ttl)) // 2, ttl)
        win.addstr(by + 2, bx + (bw - mlen) // 2, msg, curses.A_BOLD)
    except curses.error:
        pass
    win.noutrefresh()
    curses.doupdate()


def show_output_file(win, saved_as, total_bytes):
    R, C = win.getmaxyx()
    win.erase()
    win.box()
    _safe_addstr(win, 0, 2, f" OUTPUT — {saved_as} ", curses.A_BOLD)
    max_r, max_c = R - 2, C - 4
    r = 1
    try:
        with open(saved_as, errors='replace') as f:
            for line in f:
                if r > max_r:
                    tail = (f"... {total_bytes} bytes total — "
                            f"see {saved_as} for full output")
                    _safe_addstr(win, R - 1, 2, tail[:max_c], curses.A_DIM)
                    break
                _safe_addstr(win, r, 2, line.rstrip('\n\r')[:max_c])
                r += 1
    except OSError:
        _safe_addstr(win, 1, 2, f"(could not open {saved_as})")
    win.noutrefresh()


def refresh_all(hdr, mnu, cnt, sts, sel):
    draw_header(hdr)
    draw_menu(mnu, sel)
    for w in (cnt, sts):
        w.touchwin()
        w.noutrefresh()
    curses.doupdate()

# ── "Run Code" dialog ────────────────────────────────────────────────────────


def do_run_code(sock, hdr, mnu, cnt, sts, sel, hint):
    """
    Blocking dialog: collects filename, time limit, and memory limit from the
    user, then sends the request and returns True (waiting) or False
    (cancelled / error) immediately.  The response arrives asynchronously.
    """
    dh, dw = 11, 62
    dy = max(0, (curses.LINES - dh) // 2)
    dx = max(0, (curses.COLS - dw) // 2)
    dlg = curses.newwin(dh, dw, dy, dx)
    dlg.box()
    _safe_addstr(dlg, 0, (dw - 12) // 2, " RUN CODE ", curses.A_BOLD)
    _safe_addstr(dlg, 2, 3, "Source file path (blank to cancel):")
    _safe_addstr(dlg, 3, 3, "> ")
    _safe_addstr(dlg, 5, 3, "Time limit in seconds (0 = no limit):")
    _safe_addstr(dlg, 6, 3, "> ")
    _safe_addstr(dlg, 8, 3, "Memory limit in MB (0 = no limit):")
    _safe_addstr(dlg, 9, 3, "> ")
    dlg.refresh()

    curses.echo()
    curses.curs_set(1)
    try:
        filename = dlg.getstr(3, 5, 255).decode(errors='replace').strip()
        tlimit_s = dlg.getstr(6, 5, 10).decode(errors='replace').strip()
        mlimit_mb = dlg.getstr(9, 5, 10).decode(errors='replace').strip()
    except Exception:
        filename = ""
        tlimit_s = "0"
        mlimit_mb = "0"
    finally:
        curses.noecho()
        curses.curs_set(0)

    del dlg
    refresh_all(hdr, mnu, cnt, sts, sel)

    if not filename:
        set_status(sts, "Cancelled.")
        curses.doupdate()
        return False

    lang = detect_language(filename)
    if not lang:
        set_status(sts, "Unsupported extension. Use .c .py .cpp .java", err=True)
        curses.doupdate()
        return False

    try:
        code_size = os.path.getsize(filename)
    except OSError as e:
        set_status(sts, f"Cannot stat '{filename}': {e}", err=True)
        curses.doupdate()
        return False

    try:
        time_limit = max(0, int(tlimit_s))
    except ValueError:
        time_limit = 0
    try:
        mem_limit = max(0, int(mlimit_mb))
    except ValueError:
        mem_limit = 0

    out_name = make_output_name(filename)

    # Set hint BEFORE the first byte hits the wire
    with hint['lock']:
        hint['is_file'] = True
        hint['save_path'] = out_name

    fname_b = filename.encode()[:64].ljust(64, b'\x00')
    header = struct.pack(REQ_FMT, lang, code_size,
                         fname_b, time_limit, mem_limit)

    show_info_box(cnt, f"Sending {code_size} bytes ({lang_name(lang)})…")
    set_status(sts, f"Sending {code_size} bytes ({lang_name(lang)})…")
    curses.doupdate()

    # Stream file in chunks — never loads the whole file into memory
    _log.info("sending %s lang=%d size=%d", filename, lang, code_size)
    try:
        sock.sendall(header)
        with open(filename, 'rb') as f:
            while True:
                chunk = f.read(65536)
                if not chunk:
                    break
                sock.sendall(chunk)
    except OSError as e:
        _log.error("send error: %s", e)
        set_status(sts, f"Send error: {e}", err=True)
        curses.doupdate()
        return False

    show_info_box(cnt, "Executing on server — please wait…")
    set_status(sts, "Executing on server — please wait…")
    curses.doupdate()
    return True   # now waiting for async response

# ── main curses function ─────────────────────────────────────────────────────


def run(stdscr):
    locale.setlocale(locale.LC_ALL, '')
    curses.curs_set(0)
    init_colors()

    R, C = stdscr.getmaxyx()
    hdr = curses.newwin(1,        C,           0,      0)
    mnu = curses.newwin(R - 2,    MENU_W,      1,      0)
    cnt = curses.newwin(R - 2,    C - MENU_W,  1,      MENU_W)
    sts = curses.newwin(1,        C,           R - 1,  0)
    mnu.keypad(True)

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        _log.info("connected to %s:%d", HOST, PORT)
    except Exception as e:
        _log.error("connection failed: %s", e)
        curses.endwin()
        sys.exit(f"Connection to {HOST}:{PORT} failed: {e}")

    resp_queue = queue.Queue()
    hint = {'lock': threading.Lock(), 'is_file': False, 'save_path': ''}
    threading.Thread(
        target=recv_thread, args=(s, resp_queue, hint), daemon=True
    ).start()

    draw_header(hdr)
    sel = 0
    draw_menu(mnu, sel)
    show_content(cnt, "OUTPUT",
                 "Select an action from the menu.\n\n"
                 "  Run Code      — compile and run a source file\n"
                 "  Ping Server   — check connectivity and latency\n"
                 "  Languages     — list supported languages\n"
                 "  Server Status — uptime and connections\n"
                 "  Queue Depth   — pending task count\n"
                 "  Who Am I      — your client ID and address\n"
                 "  Help          — full command reference\n")
    set_status(sts, "Arrow keys: navigate   Enter: select   q: quit")
    curses.doupdate()

    waiting = False
    ping_t0 = 0.0
    pending_sel = -1

    # 50ms getch timeout: returns -1 when no key, letting the loop drain
    # resp_queue without blocking indefinitely on user input.
    mnu.timeout(50)

    try:
        while True:
            ch = mnu.getch()

            # ── poll for completed async response ──
            try:
                resp = resp_queue.get_nowait()
            except queue.Empty:
                resp = None

            if resp is not None:
                waiting = False
                if resp.get('error'):
                    set_status(sts, "Connection lost.", err=True)
                elif resp.get('is_file'):
                    show_output_file(cnt, resp['save_path'], resp['bytes'])
                    saved = resp['save_path']
                    sz = resp['bytes']
                    set_status(sts,
                               f"Saved '{saved}'  ({sz} bytes)  — "
                               "Arrow keys: navigate   q: quit")
                else:
                    text = resp.get('text', '')
                    if pending_sel == 1:   # PING: append measured RTT
                        ms = int((time.monotonic() - ping_t0) * 1000)
                        text = f"{text}\nRound-trip latency: {ms} ms\n"
                    title = TITLE_MAP.get(pending_sel, "RESPONSE")
                    show_content(cnt, title, text)
                    set_status(
                        sts, "Arrow keys: navigate   Enter: select   q: quit")
                draw_menu(mnu, sel)
                curses.doupdate()

            if ch == -1:   # getch timeout — no key this cycle
                continue

            if ch == curses.KEY_UP:
                sel = (sel - 1) % len(MENU_ITEMS)
                draw_menu(mnu, sel)
                curses.doupdate()
                continue

            if ch == curses.KEY_DOWN:
                sel = (sel + 1) % len(MENU_ITEMS)
                draw_menu(mnu, sel)
                curses.doupdate()
                continue

            if ch in (ord('q'), ord('Q')):
                break

            if ch not in (curses.KEY_ENTER, ord('\n'), ord('\r')):
                continue

            # ── Enter pressed ──
            if waiting:
                show_info_box(cnt, "Waiting for server response…")
                set_status(sts, "Waiting for server response…")
                curses.doupdate()
                continue

            if sel == 0:
                waiting = do_run_code(s, hdr, mnu, cnt, sts, sel, hint)
                if waiting:
                    pending_sel = 0
                draw_menu(mnu, sel)
                curses.doupdate()

            elif sel == 7:
                break

            elif 1 <= sel <= 6:
                # Set hint before sending
                with hint['lock']:
                    hint['is_file'] = False
                    hint['save_path'] = ''
                if sel == 1:
                    ping_t0 = time.monotonic()
                pending_sel = sel
                waiting = True
                cmd_b = CMD_MAP[sel].encode()[:64].ljust(64, b'\x00')
                s.sendall(struct.pack(REQ_FMT, 0xFF, 0, cmd_b, 0, 0))
                show_info_box(cnt, "Loading…")
                set_status(sts, "Loading…")
                curses.doupdate()

    except (ConnectionError, OSError) as e:
        curses.endwin()
        sys.exit(f"Network error: {e}")
    finally:
        s.close()


if __name__ == '__main__':
    curses.wrapper(run)
