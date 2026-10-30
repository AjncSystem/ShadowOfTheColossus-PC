"""SotC Lab control pipe: line framing and reply parsing of tools/sotc_ctl.py (no game needed).

Pipe I/O is only exercised when SOTC_CTL_LIVE=1 names a running game (default pipe sotc-ctl, or
SOTC_CTL_PIPE for another name).
"""
from paths import ROOT
import importlib.util
import os
import unittest

_spec = importlib.util.spec_from_file_location('sotc_ctl', ROOT / 'tools' / 'sotc_ctl.py')
ctl = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ctl)

STATE_REPLY = (
    '{"pos":[1.000,2.000,3.000],"cam":[0.000,1.000,2.000,0.500,-0.250],"freecam":false,'
    '"autopilot":{"loaded":true,"playing":false,"recording":false,"loop":true,"point":2,'
    '"count":5,"seg":"bridge","lap":0,"distance_done":4.000,"distance_total":10.000},'
    '"area":"shrine","boss_pending":false,"segment":"bridge",'
    '"window":{"frames":120,"p50":16.667,"p99":33.333,"max":41.000,"hitch33":1,"hitch50":0,'
    '"hitch100":0},"live":{"SOTC_LOD_LINGER":1},"pending":{"SOTC_FOO":"2"},'
    '"relaunch":{"requested":false,"boss":0}}'
)


class LineReaderTests(unittest.TestCase):
    def test_splits_lines_across_chunks(self):
        reader = ctl.LineReader()
        self.assertEqual(reader.feed(b'ok'), [])
        self.assertEqual(reader.feed(b' pong\nerr '), ['ok pong'])
        self.assertEqual(reader.feed(b'bad-value\n'), ['err bad-value'])
        self.assertEqual(reader.pending(), b'')

    def test_keeps_partial_line(self):
        reader = ctl.LineReader()
        self.assertEqual(reader.feed(b'{"a":1'), [])
        self.assertEqual(reader.pending(), b'{"a":1')
        self.assertEqual(reader.feed(b'}\n'), ['{"a":1}'])

    def test_crlf_and_several_lines_in_one_chunk(self):
        reader = ctl.LineReader()
        self.assertEqual(reader.feed(b'ok\r\nok pong\r\n'), ['ok', 'ok pong'])

    def test_bytes_one_at_a_time(self):
        reader = ctl.LineReader()
        lines = []
        for byte in b'ok\nerr x\n':
            lines += reader.feed(bytes([byte]))
        self.assertEqual(lines, ['ok', 'err x'])


class ReplyTests(unittest.TestCase):
    def test_ok_and_err(self):
        self.assertEqual(ctl.parse_reply('ok'), ('ok', ''))
        self.assertEqual(ctl.parse_reply('ok pong'), ('ok', 'pong'))
        self.assertEqual(ctl.parse_reply('err needs-restart'), ('err', 'needs-restart'))
        self.assertTrue(ctl.is_error('err live-unavailable relaunch'))
        self.assertFalse(ctl.is_error('ok pong'))

    def test_unknown_reply_raises(self):
        with self.assertRaises(ctl.CtlError):
            ctl.parse_reply('hello')

    def test_broken_json_raises(self):
        with self.assertRaises(ctl.CtlError):
            ctl.parse_reply('{"pos":')

    def test_parse_state(self):
        state = ctl.parse_state(STATE_REPLY)
        self.assertEqual(state['pos'], [1.0, 2.0, 3.0])
        self.assertEqual(state['cam'][3:], [0.5, -0.25])
        self.assertFalse(state['freecam'])
        self.assertEqual(state['autopilot']['seg'], 'bridge')
        self.assertEqual(state['window']['frames'], 120)
        self.assertEqual(state['live'], {'SOTC_LOD_LINGER': 1})
        self.assertEqual(state['pending'], {'SOTC_FOO': '2'})
        self.assertEqual(state['relaunch'], {'requested': False, 'boss': 0})

    def test_parse_state_rejects_plain_reply(self):
        with self.assertRaises(ctl.CtlError):
            ctl.parse_state('ok pong')

    def test_json_string_escapes_decode(self):
        kind, value = ctl.parse_reply('{"area":"a\\"b\\\\c\\u0001"}')
        self.assertEqual(kind, 'json')
        self.assertEqual(value['area'], 'a"b\\c\x01')


class FramingTests(unittest.TestCase):
    def test_format_line_strips_and_rejects_newlines(self):
        self.assertEqual(ctl.format_line('  get state \n'), 'get state')
        with self.assertRaises(ValueError):
            ctl.format_line('ping\nquit')
        with self.assertRaises(ValueError):
            ctl.format_line('   ')

    def test_pipe_path(self):
        self.assertEqual(ctl.pipe_path('sotc-ctl'), '\\\\.\\pipe\\sotc-ctl')
        self.assertEqual(ctl.pipe_path('\\\\.\\pipe\\other'), '\\\\.\\pipe\\other')


@unittest.skipUnless(os.environ.get('SOTC_CTL_LIVE') == '1', 'needs a running game with SOTC_CTL')
class LivePipeTests(unittest.TestCase):
    def test_ping(self):
        with ctl.Ctl(os.environ.get('SOTC_CTL_PIPE', ctl.DEFAULT_PIPE)) as client:
            self.assertEqual(client.send('ping'), 'ok pong')
            ctl.parse_state(client.send('get state'))


if __name__ == '__main__':
    unittest.main()
