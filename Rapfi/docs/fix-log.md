# Fix log

Append-only log of bugs where an early fix touched the layer where the symptom was *visible*
while the layer that actually *produces* the wrong value stayed wrong (see `../CLAUDE.md`,
"Root-cause diagnosis"). One entry per occurrence, newest last.

Entry format: date, symptom, the fix that was first applied, where the value really came from,
what the corrected fix was, and the check that would have caught it earlier.

---

*(no entries yet)*
