#!/usr/bin/env python3
"""Client for the SotC Lab control pipe (gpu/sotc/ctl_server.cpp, commands in gpu/sotc/lab_commands.h).

usage:
  python tools/sotc_ctl.py [--pipe NAME] "get state" "freecam on"    one reply line per command
  python tools/sotc_ctl.py [--pipe NAME]                             commands read from stdin

Importable: Ctl(pipe).send(line) -> reply string (the connection stays open between calls).
Exit status: 0 when every reply is not an error, 1 when one is ("err ..."), 2 when the pipe fails.
Stdlib only. The pipe is \\\\.\\pipe\\<NAME> (default "sotc-ctl", the game's SOTC_CTL value).
"""
import argparse
import json
import os
import sys

DEFAULT_PIPE = 'sotc-ctl'
PIPE_PREFIX = '\\\\.\\pipe\\'


class CtlError(RuntimeError):
    """The pipe failed or a reply could not be parsed."""


def pipe_path(name):
    """Full Windows path of a pipe name (a full path is returned unchanged)."""
    if name.startswith(PIPE_PREFIX):
        return name
    return PIPE_PREFIX + name


def format_line(line):
    """One command line as sent on the wire. Commands are single lines."""
    text = line.strip()
    if not text:
        raise ValueError('empty command')
    if '\n' in text or '\r' in text:
        raise ValueError('a command must be one line')
    return text


class LineReader:
    """Splits a byte stream into lines. Feed it whatever chunks arrive; partial lines are kept."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data):
        """Adds bytes and returns the complete lines (decoded, without CR/LF)."""
        self._buf.extend(data)
        lines = []
        while True:
            nl = self._buf.find(b'\n')
            if nl < 0:
                break
            raw = bytes(self._buf[:nl])
            del self._buf[:nl + 1]
            lines.append(raw.rstrip(b'\r').decode('utf-8', errors='replace'))
        return lines

    def pending(self):
        """Bytes received without a terminating newline yet."""
        return bytes(self._buf)


def parse_reply(reply):
    """Classifies a reply: ('ok', text), ('err', text) or ('json', object).

    'ok ...' and 'err ...' replies are returned as their text. A reply starting with '{' is
    decoded as JSON (see get state / get live in lab_commands.h)."""
    text = reply.strip()
    if text == 'ok' or text.startswith('ok '):
        return 'ok', text[2:].strip()
    if text == 'err' or text.startswith('err '):
        return 'err', text[3:].strip()
    if text.startswith('{'):
        try:
            return 'json', json.loads(text)
        except json.JSONDecodeError as error:
            raise CtlError(f'bad JSON reply: {error}') from error
    raise CtlError(f'unrecognised reply: {text!r}')


def parse_state(reply):
    """The dict of a 'get state' reply. Raises CtlError when the reply is not JSON."""
    kind, value = parse_reply(reply)
    if kind != 'json':
        raise CtlError(f'expected JSON, got: {reply.strip()!r}')
    return value


def is_error(reply):
    return reply.strip().startswith('err')


class Ctl:
    """A connection to the control pipe. Windows only; the game must run with SOTC_CTL or SOTC_LAB."""

    def __init__(self, pipe=DEFAULT_PIPE):
        self.pipe = pipe
        self._file = None
        self._reader = LineReader()

    def _open(self):
        if self._file is None:
            # Unbuffered: every byte goes straight to the pipe, so a reply is never read past.
            self._file = open(pipe_path(self.pipe), 'r+b', buffering=0)
        return self._file

    def close(self):
        if self._file is not None:
            try:
                self._file.close()
            finally:
                self._file = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def send(self, line):
        """Sends one command and returns its reply line (no trailing newline)."""
        command = format_line(line)
        try:
            stream = self._open()
            stream.write((command + '\n').encode('utf-8'))
            while True:
                chunk = stream.read(1)
                if not chunk:
                    raise CtlError('pipe closed by the game')
                lines = self._reader.feed(chunk)
                if lines:
                    return lines[0]
        except OSError as error:
            self.close()
            raise CtlError(f'pipe {self.pipe!r} failed: {error}') from error


def main(argv=None):
    parser = argparse.ArgumentParser(description='Send commands to the SotC Lab control pipe.')
    parser.add_argument('--pipe', default=os.environ.get('SOTC_CTL') or DEFAULT_PIPE,
                        help='pipe name (default: $SOTC_CTL or sotc-ctl)')
    parser.add_argument('commands', nargs='*', help='one command per argument')
    args = parser.parse_args(argv)
    if args.pipe in ('1', '0'):
        args.pipe = DEFAULT_PIPE

    commands = args.commands or [line for line in sys.stdin.read().splitlines() if line.strip()]
    status = 0
    try:
        with Ctl(args.pipe) as ctl:
            for command in commands:
                reply = ctl.send(command)
                print(reply)
                if is_error(reply):
                    status = 1
    except (CtlError, ValueError) as error:
        print(f'err {error}', file=sys.stderr)
        return 2
    return status


if __name__ == '__main__':
    sys.exit(main())
