# AutoFPCount

A frame-window analyser and macro recorder for Geometry Dash 2.2081, built on the
Geode SDK.

AutoFPCount imports macros from Silicate, XDBot, Mega Hack and Eclipse, enforces a
strict 240 FPS ingestion gate, replays what it accepts, and for every press draws
the exact grid position, the number of ticks of slack it had, and the frame-rate
ceiling that slack corresponds to (`240 / window` on a 240 FPS baseline).

It also maintains a live `Frame Window: Count` tally in the screen corner, grouped
into the fixed buckets `1`, `2`, `3`, `4`, `5-6`, `7-8`, `9-10`, `11-12`.

See the repository `README.md` for building and usage, and `docs/FORMATS.md` for
the wire-format schemas and the precise limits of the window maths.