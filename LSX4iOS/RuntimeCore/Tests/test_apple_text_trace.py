"""Behavioral host validation of the separate system IME trace format."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apple_text_trace import validate_text_trace


class TextTraceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'trace.json'

    def read(self, events, duration=60):
        self.path.write_text(json.dumps(events), encoding='utf-8')
        return validate_text_trace(self.path, duration)

    def event(self, **changes):
        return {'seconds': 1, 'session': 'name', 'action': 'text', 'text': 'AЖ', **changes}

    def test_unicode_and_ordered_actions(self):
        events=[self.event(), {'seconds': 2, 'session': 'name', 'action': 'backspace', 'count': 1},
                self.event(seconds=3,text='界'), {'seconds': 4,'session':'name','action':'submit'}]
        self.assertEqual(self.read(events), events)

    def test_new_labels_and_cancel(self):
        events=[{'seconds': 0,'session':'a','action':'cancel'},self.event(seconds=0,session='b')]
        self.assertEqual(self.read(events), events)

    def test_invalid_time(self):
        for value in (True, None, -1, 60, float('nan'), float('inf'), '1'):
            with self.subTest(value=value), self.assertRaises(ValueError): self.read([self.event(seconds=value)])
        with self.assertRaises(ValueError): self.read([self.event(seconds=2), self.event(seconds=1)])

    def test_invalid_schema(self):
        for value in ({}, None, 'trace', [None], [self.event(extra=1)], [self.event(action='Text')]):
            with self.subTest(value=value), self.assertRaises(ValueError): self.read(value)

    def test_invalid_labels(self):
        for label in ('', 'x'*65, 'a b', 'а', 1):
            with self.subTest(label=label), self.assertRaises(ValueError): self.read([self.event(session=label)])

    def test_text_boundary_and_invalid_unicode(self):
        self.read([self.event(text='界'*5461+'a')]) # Exactly 16384 UTF-8 bytes.
        for value in ('', '\0', 'a\0b', '\ud800', '\udfff', '界'*5462, None):
            with self.subTest(value=repr(value)[:30]), self.assertRaises(ValueError): self.read([self.event(text=value)])

    def test_backspace_boundary_and_integer(self):
        for count in (1, 1.0, 16384):
            self.read([{'seconds': 1,'session':'a','action':'backspace','count':count}])
        for count in (True, None, 0, 16385, 1.5, float('nan'), float('inf')):
            with self.subTest(count=count), self.assertRaises(ValueError):
                self.read([{'seconds':1,'session':'a','action':'backspace','count':count}])

    def test_reject_cross_action_fields(self):
        for event in (self.event(count=1), self.event(action='backspace',count=1), self.event(action='submit'),
                      {'seconds':1,'session':'a','action':'cancel','count':1}):
            with self.subTest(event=event), self.assertRaises(ValueError): self.read([event])

    def test_terminal_label_cannot_rebind(self):
        for action in ('submit','cancel'):
            with self.assertRaises(ValueError):
                self.read([{'seconds':1,'session':'name','action':action},self.event(seconds=2)])

    def test_limits_and_empty(self):
        self.assertEqual(self.read([]), [])
        self.read([self.event()]*128)
        with self.assertRaises(ValueError): self.read([self.event()]*129)
        self.path.write_bytes(b' '*(1024*1024+1))
        with self.assertRaises(ValueError): validate_text_trace(self.path,60)

    def test_invalid_file(self):
        with self.assertRaises(OSError): validate_text_trace(self.path,60)
        self.path.write_bytes(b'\xff')
        with self.assertRaises(ValueError): validate_text_trace(self.path,60)


if __name__ == '__main__': unittest.main(verbosity=2)
