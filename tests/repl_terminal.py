#!/usr/bin/env python3
# The interactive prompt in a real terminal (a pseudo-terminal): an
# expression's value, Ctrl-C abandoning a line and a multi-line input, a
# pasted block arriving whole, tab completion, and Ctrl-D leaving.
#
#   python3 repl_terminal.py <cluau>
#
# Each step waits for the prompt's own output (no fixed delays), so a slow
# machine only makes it slower.

import os
import pty
import re
import select
import sys
import tempfile
import time

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")


class Prompt:
    def __init__(self, exe):
        env = dict(os.environ, HOME=tempfile.mkdtemp(prefix="cluau-repl-"), TERM="xterm")
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(exe, [exe], env)
        self.seen = ""

    def expect(self, pattern, what, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            match = re.search(pattern, self.seen)
            if match:
                self.seen = self.seen[match.end():]
                return
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(self.fd, 4096)
                except OSError:
                    break
                if not chunk:
                    break
                self.seen += ANSI.sub(b"", chunk).decode(errors="replace")
        sys.exit(f"FAIL {what}: no {pattern!r} in the output; it ends with {self.seen[-300:]!r}")

    def send(self, text):
        os.write(self.fd, text.encode())

    def finish(self):
        _, status = os.waitpid(self.pid, 0)
        return os.waitstatus_to_exitcode(status)


def main():
    p = Prompt(sys.argv[1])
    p.expect(r"Ctrl-D exits\.", "the banner")

    p.send("6 * 7\r")
    p.expect(r"\n42\r?\n", "an expression's value")

    p.send("this line is abandoned\x03")
    p.send("1 + 1\r")
    p.expect(r"\n2\r?\n", "Ctrl-C abandoning a line")

    p.send("if (true) {\r")
    p.expect(r">> ", "a statement continuing")
    p.send("\x03")
    p.send("2 + 2\r")
    p.expect(r"\n4\r?\n", "Ctrl-C abandoning a statement over several lines")

    p.send("if (true) {\r print('pasted')\r }\r")
    p.expect(r"\npasted\r?\n", "a pasted block arriving whole")

    p.send("string.up\t")
    p.expect(r"string\.upper\(", "tab completing a module's member")
    p.send("'x')\r")
    p.expect(r'"X"', "the completed call")

    p.send("class Pet {\r function Speak(): string { return 'hi' }\r }\r")
    p.send("class Dog extends Pet { }\r")
    p.send("let rex = Dog()\r")
    p.send("rex:Sp\t")
    p.expect(r"rex:Speak\(", "tab completing an inherited method")
    p.send(")\r")
    p.expect(r'"hi"', "the completed method call")

    p.send("\x04")
    status = p.finish()
    if status != 0:
        sys.exit(f"FAIL Ctrl-D: the prompt exited with {status}")
    print("repl_terminal: every step passed")


main()
