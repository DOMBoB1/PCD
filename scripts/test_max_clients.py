#!/usr/bin/env python3
"""
test_max_clients.py — verify the server's max-clients enforcement.

Connects cfg_max_clients (default 1024) sockets, keeps them all open,
then tries one more and checks the response.

Usage:
    python3 scripts/test_max_clients.py [host [port [target]]]

    host    server address   (default 127.0.0.1)
    port    server port      (default 8080)
    target  number of "good" connections to attempt  (default 1024)
"""
import resource
import socket
import sys
import time

HOST   = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
PORT   = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
TARGET = int(sys.argv[3]) if len(sys.argv) > 3 else 1024

TIMEOUT_CONNECT = 5.0   # seconds to wait for TCP handshake
TIMEOUT_BANNER  = 0.3   # seconds to wait for an immediate server message


# ── helpers ───────────────────────────────────────────────────────────────────

def raise_fd_limit(needed: int) -> None:
    """Raise the process fd limit so we can hold TARGET open sockets."""
    try:
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        want = needed + 64          # leave headroom for stdio + misc
        if soft < want:
            new_soft = min(want, hard if hard > 0 else want)
            resource.setrlimit(resource.RLIMIT_NOFILE, (new_soft, hard))
            print(f"[fd-limit] raised {soft} → {new_soft}  (hard cap: {hard})")
        else:
            print(f"[fd-limit] {soft} available — no change needed")
    except Exception as e:
        print(f"[fd-limit] WARNING: could not raise fd limit: {e}")
        print(f"           Run: ulimit -n {needed + 64}  before this script")


def try_connect(label: str) -> tuple:
    """
    Try one TCP connection to (HOST, PORT).

    Returns (socket | None, banner_str | None, error_str | None).
    banner_str is set only when the server sends something within TIMEOUT_BANNER.
    """
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(TIMEOUT_CONNECT)
        s.connect((HOST, PORT))
        s.settimeout(TIMEOUT_BANNER)
        try:
            raw = s.recv(64)
            banner = raw.decode(errors='replace').strip() if raw else None
        except socket.timeout:
            banner = None      # normal: server doesn't greet regular clients
        return s, banner, None
    except Exception as exc:
        return None, None, str(exc)


# ── main ──────────────────────────────────────────────────────────────────────

def main() -> None:
    print("=" * 60)
    print(f"  Server : {HOST}:{PORT}")
    print(f"  Target : connect {TARGET} clients, then attempt #{TARGET + 1}")
    print("=" * 60)

    raise_fd_limit(TARGET)
    print()

    sockets      : list  = []
    server_fulls : list  = []   # indices where we got SERVER_FULL early
    os_refused   : list  = []   # indices where the OS itself refused
    t0 = time.monotonic()

    for i in range(TARGET):
        n = i + 1
        s, banner, err = try_connect(f"#{n}")

        if err:
            os_refused.append(n)
            if len(os_refused) <= 3 or n == TARGET:
                print(f"  #{n:5d}  OS error: {err}")
            elif len(os_refused) == 4:
                print(f"  #{n:5d}  … (further OS errors suppressed)")
        elif banner and 'FULL' in banner.upper():
            server_fulls.append(n)
            s.close()
            print(f"  #{n:5d}  SERVER_FULL received (unexpected this early)")
        else:
            sockets.append(s)

        # progress every 100 attempts
        if n % 100 == 0:
            elapsed = time.monotonic() - t0
            print(f"  #{n:5d}  open={len(sockets):4d}  "
                  f"srv_full={len(server_fulls)}  "
                  f"os_err={len(os_refused)}  "
                  f"[{elapsed:.1f}s]")

    elapsed = time.monotonic() - t0
    connected = len(sockets)

    print()
    print("─" * 60)
    print(f"  After {TARGET} attempts ({elapsed:.2f}s):")
    print(f"    ✓  Kept open       : {connected}")
    print(f"    ✗  SERVER_FULL     : {len(server_fulls)}")
    print(f"    ✗  OS refused      : {len(os_refused)}")
    print("─" * 60)
    print()

    # ── attempt the extra connection ──────────────────────────────────────────
    n_extra = TARGET + 1
    print(f"Attempting client #{n_extra} (the one that should be refused) …")
    s_extra, banner_extra, err_extra = try_connect(f"#{n_extra}")

    if err_extra:
        print(f"  #{n_extra:5d}  OS-level connection refused: {err_extra}")
        result = "OS refused"
    elif banner_extra and 'FULL' in banner_extra.upper():
        print(f"  #{n_extra:5d}  ✓  Got SERVER_FULL — server correctly enforced the limit")
        s_extra.close()
        result = "SERVER_FULL"
    elif banner_extra:
        print(f"  #{n_extra:5d}  Unexpected banner: {banner_extra!r}")
        s_extra.close()
        result = f"banner={banner_extra!r}"
    else:
        # Connected but no banner — server may allow more than TARGET
        print(f"  #{n_extra:5d}  Connected without rejection "
              f"(server may allow more than {TARGET} clients)")
        sockets.append(s_extra)
        result = "accepted (no limit hit)"

    # ── summary ───────────────────────────────────────────────────────────────
    print()
    print("=" * 60)
    print("  SUMMARY")
    print("=" * 60)
    print(f"  Connections kept open : {connected}")
    print(f"  Client #{n_extra} result      : {result}")

    if len(server_fulls):
        print(f"\n  NOTE: SERVER_FULL appeared at connection(s): {server_fulls[:5]}")
        print(f"  This may indicate the server's FD_SETSIZE is lower than")
        print(f"  cfg_max_clients (select() cannot monitor fds >= FD_SETSIZE).")

    if len(os_refused):
        print(f"\n  NOTE: {len(os_refused)} OS-level refusals occurred.")
        print(f"  Raise the fd limit:  ulimit -n {TARGET + 64}")

    # ── teardown ──────────────────────────────────────────────────────────────
    print(f"\nClosing {len(sockets)} open socket(s) …")
    for s in sockets:
        try:
            s.close()
        except Exception:
            pass
    print("Done.")


if __name__ == '__main__':
    main()
