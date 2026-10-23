"""Validate the opt-in, session-bound system IME trace before device setup."""
import json
import math
from pathlib import Path
import re


def validate_text_trace(path: Path, duration: int):
    raw = path.read_bytes()
    if len(raw) > 1024 * 1024:
        raise ValueError('Text trace exceeds 1 MiB')
    events = json.loads(raw)
    if not isinstance(events, list) or len(events) > 128:
        raise ValueError('Text trace must be an array of at most 128 events')
    previous = 0
    terminal = set()
    for index, event in enumerate(events, 1):
        def invalid(reason):
            raise ValueError(f'Text trace event {index}: {reason}')
        if not isinstance(event, dict) or set(event) - {'seconds', 'session', 'action', 'text', 'count'}:
            invalid('invalid fields')
        when = event.get('seconds')
        if type(when) not in (int, float) or not math.isfinite(when) or not previous <= when < duration:
            invalid('time must be ordered and within the observation duration')
        previous = when
        session = event.get('session')
        if not isinstance(session, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', session):
            invalid('invalid session label')
        if session in terminal:
            invalid('session label reused after submit/cancel')
        action = event.get('action')
        if action == 'text':
            value = event.get('text')
            if 'count' in event or not isinstance(value, str) or not value or '\0' in value:
                invalid('text must be nonempty UTF-8 without NUL')
            try:
                encoded = value.encode('utf-8', errors='strict')
            except UnicodeError:
                invalid('invalid Unicode text')
            if len(encoded) > 16384:
                invalid('text exceeds 16384 UTF-8 bytes')
        elif action == 'backspace':
            count = event.get('count')
            if ('text' in event or type(count) not in (int, float) or not math.isfinite(count)
                    or count != math.floor(count) or not 1 <= count <= 16384):
                invalid('backspace requires an integer count in 1–16384')
        elif action in ('submit', 'cancel'):
            if 'text' in event or 'count' in event:
                invalid('terminal action cannot carry text/count')
            terminal.add(session)
        else:
            invalid('unknown action')
    return events
