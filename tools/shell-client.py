#!/usr/bin/env python3
"""Drive a Cooja-NG shell started with --shell-port --shell-json (docs/shell.md).

    ./build/test_runner test configs/shell-nrf54l15-dk.yaml -q --shell-port 7000 --shell-json &
    tools/shell-client.py 7000 'wait-until 2s' 'cmd 1 ip-addr'

Each command is sent after the previous one's "done" record.  After the last
command the connection is closed, which ends the run, and the client exits
with the run's exit status.  Without commands, lines are read from stdin.
Records are printed as text, or as the raw JSON lines with --json.
"""
import json
import socket
import sys


def show(line, rec, raw):
    if raw:
        print(line.rstrip("\n"))
    elif rec["type"] == "out":
        print(rec["text"])
    elif rec["type"] == "console":
        print("  %7.3f [Node %d/%s] %s" % (rec["t"], rec["node"], rec["kind"], rec["line"]))
    elif rec["type"] == "error":
        print("error: %s" % rec["text"])
    elif rec["type"] == "done" and not rec["ok"]:
        print("failed: %s -- %s" % (rec["line"], rec.get("error", "")))
    elif rec["type"] == "result":
        print("result: status %d%s" % (rec["status"],
              ", " + rec["verdict"] if "verdict" in rec else ""))
    sys.stdout.flush()


def main():
    args = sys.argv[1:]
    raw = "--json" in args
    args = [a for a in args if a != "--json"]
    if not args:
        print(__doc__.strip())
        return 2
    port, commands = int(args[0]), args[1:]
    sock = socket.create_connection(("127.0.0.1", port))
    f = sock.makefile("rw", encoding="utf-8", errors="replace", newline="\n")
    status = 1

    def until_done():
        nonlocal status
        for line in f:
            if not line.startswith("{"):
                continue
            rec = json.loads(line)
            show(line, rec, raw)
            if rec["type"] == "result":
                status = rec["status"]
                return False
            if rec["type"] == "done":
                return True
        return False

    source = commands if commands else (l.rstrip("\n") for l in sys.stdin)
    alive = True
    for cmd in source:
        f.write(cmd + "\n")
        f.flush()
        if not until_done():
            alive = False
            break
    if alive:
        sock.shutdown(socket.SHUT_WR)       # end of input ends the run
        while until_done():
            pass
    return status


if __name__ == "__main__":
    sys.exit(main())
