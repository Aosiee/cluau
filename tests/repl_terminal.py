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
import signal
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

    # The prompt is back (its line editor reads keys again): a control key
    # sent before that reaches the terminal's own line handling instead.
    def ready(self, what, continuing=False):
        self.expect(r">> \r?$" if continuing else r"(?<!>)> \r?$", what)

    def finish(self):
        _, status = os.waitpid(self.pid, 0)
        return os.waitstatus_to_exitcode(status)


def main():
    # Whatever happens, the test ends.
    signal.signal(signal.SIGALRM, lambda *_: sys.exit("FAIL: the prompt did not finish within 90 seconds"))
    signal.alarm(90)
    p = Prompt(sys.argv[1])
    p.expect(r"Ctrl-D exits\.", "the banner")
    p.ready("the first prompt")

    p.send("6 * 7\r")
    p.expect(r"\n42\r?\n", "an expression's value")
    p.ready("the prompt after a value")

    p.send("this line is abandoned")
    p.expect(r"abandoned\r?$", "the typed line")
    p.send("\x03")
    p.ready("the prompt after Ctrl-C")
    p.send("1 + 1\r")
    p.expect(r"\n2\r?\n", "Ctrl-C abandoning a line")
    p.ready("the prompt")

    p.send("if (true) {\r")
    p.ready("a statement continuing", continuing=True)
    p.send("\x03")
    p.ready("the prompt after abandoning a statement")
    p.send("2 + 2\r")
    p.expect(r"\n4\r?\n", "Ctrl-C abandoning a statement over several lines")
    p.ready("the prompt")

    p.send("if (true) {\r print('pasted')\r }\r")
    p.expect(r"\npasted\r?\n", "a pasted block arriving whole")
    p.ready("the prompt after the paste")

    p.send("string.up\t")
    p.expect(r"string\.upper\(", "tab completing a module's member")
    p.send("'x')\r")
    p.expect(r'"X"', "the completed call")
    p.ready("the prompt")

    p.send("class Pet {\r function Speak(): string { return 'hi' }\r }\r")
    p.send("class Dog extends Pet { }\r")
    p.send("let rex = Dog()\r")
    p.ready("the prompt after the classes")
    p.send("rex:Sp\t")
    p.expect(r"rex:Speak\(", "tab completing an inherited method")
    p.send(")\r")
    p.expect(r'"hi"', "the completed method call")
    p.ready("the prompt")

    p.send("\x04")
    status = p.finish()
    if status != 0:
        sys.exit(f"FAIL Ctrl-D: the prompt exited with {status}")
    print("repl_terminal: every step passed")


main()
