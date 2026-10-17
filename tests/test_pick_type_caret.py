#!/usr/bin/env python3
"""Verify the caret tracks typed text in the picker's free-text row."""
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ui_harness as H

DOGBIN = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dog")

ask = {"question": "What are you working on?", "options": [
    {"label": "A project", "description": "I am planning or scoping something"},
    {"label": "A bug", "description": "I am working on a specific issue"},
    {"label": "A learning goal", "description": "I am learning or exploring"}]}

H.SCRIPT = [{"name": "ask_user", "tool": json.dumps(ask)}, {"text": "Done."}]
d = H.start(DOGBIN, port=8191)
d.pump(3.0)
d.send("go\r")
d.pump(3.0)

# Navigate: 1 project ->2->3->4 own row (3 downs)
for _ in range(3):
    d.send(b"\x1b[B"); d.pump(0.5)

res = []

def own_row_and_caret():
    s = d.scr
    # The own row is the only line carrying the "4." label.
    for i, ln in enumerate(s.lines()):
        if "4." in ln:
            return i, ln
    return None, None

s = d.scr
own_row, own_text = own_row_and_caret()
res.append(("own row shows placeholder 'Type something'", "Type something" in (own_text or "")))
res.append(("hint is 'Type your answer' (on own row)", any("Type your answer" in l for l in s.lines())))
res.append(("cursor visible on own row", s.cursor_visible))

typed = ""
ok_tracking = True
for ch in "hello":
    typed += ch
    d.send(ch); d.pump(0.5)
    s = d.scr
    r, row = own_row_and_caret()
    charcol = row.rfind(ch)
    expected_cc = charcol + 1
    line_ok = (typed in row) and (s.cc == expected_cc) and (s.cr == r)
    print(f"typed={typed!r:8} row={row!r:45} cr={s.cr}/{r} cc={s.cc:3} expected={expected_cc:3} {'ok' if line_ok else 'MISALIGNED'}")
    if not line_ok:
        ok_tracking = False
        res.append((f"caret (row,col) tracks after typing {typed!r}", False))

res.append(("caret tracks every character", ok_tracking))
d.kill()
ok = H.report("own-row typing caret alignment", res)
sys.exit(0 if ok else 1)
