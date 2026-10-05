# Macro format schemas and analysis limits

This document records exactly what AutoFPCount's parsers expect, where each schema
came from, and where the window analysis stops being exact.

---

## 1. Provenance

The parsers were written against real implementations rather than guesswork:

| Source | What it established |
|---|---|
| [`peonii/nat-converter`](https://github.com/peonii/nat-converter) | Silicate, XDBot, xBot 2.1 and Mega Hack replay schemas. Maintained by the Silicate author; this is the reference multi-format converter. |
| [`matcool/gd-macro-converter`](https://github.com/matcool/gd-macro-converter) | Cross-check on legacy text dialects. |
| [`knnarf/gdconv`](https://github.com/knnarf/gdconv) | Cross-check on the GDR1 binary replay layout. |
| [`maxnut/GDReplayFormat`](https://github.com/maxnut/GDReplayFormat) | GDR v1 and v2 schemas, byte layouts and the input packing. Imported; see §2.7. |
| [`maxnut/GDR-converter`](https://github.com/maxnut/GDR-converter) | A real published `.gdr` capture, used as a test vector. |
| [`docs.geode-sdk.org`](https://docs.geode-sdk.org) | `PlayLayer`, `GJBaseGameLayer`, `GJGameState`, `PlayerObject`, `CCDrawNode`, `CCLabelBMFont` field and method signatures for 2.2081. |

Silicate 1.1.0 ships against GD 2.2081 with `"geode": "5.10.1"`, which is where
this mod's Geode baseline comes from.

**Eclipse's source is not public.** Its `.macro` dialect is implemented from the
long-standing community layout described below, cross-checked against every other
converter that handles it. It is the one schema here that could not be read out of
the original author's code.

---

## 2. Schemas

### 2.1 Silicate — binary

```
offset 0   f64 little endian    tick rate
offset 8   u32 little endian    input count
offset 12  count x u32 little endian
```

Each `u32` is one packed input:

| Bits | Meaning |
|---|---|
| 0 | `down` — 1 pressed, 0 released |
| 1..2 | button id; `1` is jump. Any other value is skipped. |
| 3 | player 2 |
| 4..31 | absolute frame index |

Notes:

* Silicate writes frames into the top 28 bits, so any index `> 0x0FFFFFFF` is
  dropped with a diagnostic.
* Rows whose button id is not `1` are skipped, not treated as malformed — real
  files contain them.
* Truncated files (declared count exceeds the bytes present) are rejected.

### 2.2 XDBot — text

```
line 0    <fps>
line N    <frame>|<hold>|<button>|<player2>
```

`<player2>` is absent in some exports; a missing field means player 1.

**Documented inversion.** `nat-converter`'s XDBot reader derives the player-2 flag
with `field != "1"`, which inverts it relative to that same file's writer
(`player2 as i32` → `0` for player 1 → reads back as `true`). That is a bug in
that reader, not part of the format. AutoFPCount uses the documented mapping
(`"1"` means player 2), which matches XDBot's writer.

### 2.3 xBot 2.1 — text

```
line 0    fps: <n>
line 1    frames | pro_plus
line N    <state> <frame>
```

* `hold = state % 2 == 1`
* `player2 = state > 1`

**`pro_plus` is rejected outright.** Those files store an X position instead of a
frame index, so the tick rate is not verifiable against them at all. Importing one
would mean fabricating frame numbers, which would defeat the entire point of a
240 FPS gate. The rejection message says so.

### 2.4 Mega Hack replay — JSON

```json
{
  "meta":   { "fps": 240 },
  "events": [
    { "frame": 12, "down": true, "p2": false, "x": 0, "y": 0, "a": 0, "r": 0 }
  ]
}
```

* A bare top-level array of event objects is also accepted.
* `inputs` is accepted as an alias for `events`.
* `x`, `y`, `a` and `r` are physics-correction fields. They are read and
  **discarded**: this mod drives input, not physics state. Accepting and ignoring
  them is what keeps real Mega Hack exports importable.
* A missing `meta.fps` leaves the declared rate at 0, so the gate reports
  `FpsMissing` with a message naming the real problem rather than a generic
  parse error.

### 2.5 Eclipse — text

```
line 0    <fps>
line 1    <click count>        (optional)
line N    <x>, <y>, <delay>, <down>
```

* `delay` is a **frame delta**, not an absolute index, so rows are accumulated
  into absolute ticks during parsing.
* Row shapes accepted: 4 fields (`x,y,delay,down`), 3 (`y,delay,down`) and
  2 (`delay,down`). The delay field is always second from the right.
* The count line is detected as a bare integer with no comma. A count larger than
  the rows present is a diagnostic, not an error — the rows that exist are used.
* Rows are player 1 only.

### 2.6 Plain text — text

```
line 0    <fps>
line N    <frame> <hold> <player2>
```

The trailing `<player2>` field is optional.

### 2.7 GDR - the GDevelop replay format

Decoded from [`maxnut/GDReplayFormat`](https://github.com/maxnut/GDReplayFormat) (branch
`gdr2`), reimplemented in `src/macro/GdrReader.cpp` with no dependency on its
`nlohmann/json` requirement. Two unrelated encodings share the name; **the version
is a field, not the extension**:

| Dialect | Extension | Encoding |
|---|---|---|
| v1 | `.gdr` | A serialised JSON document. Canonically MessagePack; plain JSON accepted as a fallback. |
| v2 | `.gdr2` | A hand-packed binary stream beginning with the three ASCII bytes `GDR`. |

#### v1 (`.gdr`) object schema

```
inputs[]   frame (number), btn (number), "2p" (bool), down (bool)
framerate  optional number, defaults to 240 when absent
```

Plus `author`, `description`, `gameVersion`, `duration`, `seed`, `coins`, `ldm`,
`bot{name,version}` and `level{id,name}`. Note the player-2 key is literally `"2p"`.

#### v2 (`.gdr2`) stream layout

| Field | Encoding |
|---|---|
| magic | 3 raw bytes, `"GDR"` |
| `version`, `gameVersion`, `seed`, `coins`, `bot.version`, `level.id` | varint |
| `author`, `description`, `bot.name`, `level.name`, `inputTag` | string, length-prefixed |
| `ldm`, `platformer` | varint bool |
| `duration` | `f32` **big endian** |
| `framerate` | `f64` **big endian** |
| extension block | varint size, then that many opaque bytes |
| deaths | varint count, then accumulating deltas |
| inputs | varint count, then varint `p1InputCount`, then records until EOF |

Two details in this table contradict the upstream readme, and the code was taken
as authoritative in both cases:

- **Strings are length-prefixed, not NUL terminated.** The readme says "null
  terminated string"; `binarystream.hpp` writes a varint length followed by the bytes.
- **Input packing is `delta << 3 | button << 1 | down`**, not the bit layout the
  readme describes. Platformer mode uses that form; classic mode drops the button
  and uses `delta << 1 | down`.

Fixed-width fields are big endian because `binarystream.hpp` `memcpy`s then reverses
on a little-endian host. Every integral field, `bool` included, is LEB128 varint.

The 2-player split in v2 is **positional, not per record**: the first
`p1InputCount` records belong to player 1, and the running frame delta resets to
zero at that boundary so player 2's frames are relative to its own start.

#### One deliberate divergence

The upstream reader loops until the stream is exhausted and treats the declared
input count as advisory. AutoFPCount additionally requires the decoded count to
**equal** the declared count, so a file truncated mid-record is rejected instead of
decoding into a quietly shorter replay — a macro that silently loses its last few
clicks is worse than one that fails loudly.

`tools/make_gdr_fixtures.py` emits byte-exact fixtures for every branch above and
`tools/check_gdr_reader.py` decodes them, plus a real capture published in
`maxnut/GDR-converter`. Both run in CI on every push.

### 2.8 Format detection

Content sniffing, in order:

1. first three bytes are `"GDR"` → GDR v2
2. first non-whitespace byte is `{` **and** the head contains `"inputs"`, `"bot"` or
   `"gameVersion"` → GDR v1; otherwise → Mega Hack JSON
3. starts with `fps: ` → xBot 2.1
4. first 8 bytes decode to a plausible `f64` tick rate **and** bytes 8..11 decode
   to a `u32` count that exactly accounts for the remaining payload (±3 bytes of
   padding) → Silicate binary
5. any row contains `|` → XDBot
6. any row contains `,` → Eclipse
7. a three-token row → plain text
8. otherwise → GDR v1 attempted as MessagePack, then unknown → rejected

GDR is checked before the JSON dialects because v2 has a binary magic and v1 is a
JSON document that would otherwise be mistaken for Mega Hack's — the two differ only
in which keys they use (`inputs` versus `events`).

Every text scalar is parsed with a **full-consumption** check. `strtod` stops at
the first bad character, which is how `12abc` silently becomes `12` and shifts
every subsequent field of a row.

---

## 3. The 240 FPS ingestion gate

`enforceNativeFps` rejects a macro unless **all** of these hold:

| Check | Reason code |
|---|---|
| The declared rate is finite | `FpsNotFinite` |
| The declared rate is greater than 0 | `FpsMissing` |
| The format is frame-indexed | `NotFrameBased` |
| `\|declared − 240\| <= 1e-3` | `FpsMismatch` |
| Decoded inputs ≤ 8,000,000 | `TooManyInputs` |

There is deliberately **no "closest match" path**. A macro that is genuinely a
240 FPS recording but declares `239.9994` is rejected and told exactly why.
Silence about a precision mismatch is worse than a refusal.

The gate can be turned off with the **Strict 240 FPS Gate** setting, in which
case the declared rate is logged instead.

### Ceilings

| Bound | Value | Reason |
|---|---|---|
| File size | 64 MiB | A 1-hour 240 FPS macro is ~1.5 MiB |
| Decoded inputs | 8,000,000 | Independent of file size |
| JSON nesting depth | 32 | A hostile file cannot smash the stack |
| JSON elements | 4,000,000 | Bounds one pathological array |

---

## 4. What the window number actually is

### The good part

A click's window is the number of ticks of slack before the press stops landing on
its target. AutoFPCount computes it as the distance in ticks from the click to the
nearest **interaction boundary** — a tick where the player's collision or
interaction state changed.

That is a sound construction. Those are precisely the ticks where one extra or
missing tick of input can change what the player collides with. Sampled once per
tick from plain member loads:

* four of the engine's ground flags (`m_isOnGround` .. `m_isOnGround4`)
* `m_isCollidingWithSlope` and `m_currentSlope->m_objectID`
* `m_lastGroundObject->m_objectID`
* `m_gravity`
* `m_touchedRings.size()`
* `m_isSliding`, `m_isRotating`, `m_isDead`, `m_jumpBuffered`

Boundaries are kept in a 1024-entry ring buffer, so the search is O(1) amortised
and allocation-free.

Each boundary is tagged **ground** or **other**, which is what lets the exception
engine distinguish a surface-anchored window from an orb-anchored one.

### The limitation

An exact window requires a physics roll-forward: simulate the player forward *N*
ticks with the click, simulate again without it, and compare whether the outcomes
diverge. That is what a real trajectory engine does, and it is not affordable to
run live for every click.

So this is a **proxy**. It is a tight one — it moves exactly when the physics
decision moves — but it cannot see a window whose bounds come from an interaction
that is not in the sampled set. Concretely, it will not notice:

* an orb radius that a click lands inside but that produces no sampled state
  change on the ticks either side
* a wave trigger, or a per-object collision box that differs from the surface
  the ground flags describe
* a hitbox change with no accompanying flag change

Practically: the number is right for ground, slope, and contact transitions, which
is the overwhelming majority of frame-perfect windows. Treat it as an upper bound
on slack, not an exact simulation result.

### Bucket mapping

| Raw ticks | Bucket |
|---|---|
| `<= 1` (including "no boundary known") | `1` |
| 2 | `2` |
| 3 | `3` |
| 4 | `4` |
| 5..6 | `5-6` |
| 7..8 | `7-8` |
| 9..10 | `9-10` |
| `>= 11` | `11-12` |

Windows wider than 12 ticks still increment the last bucket; the display simply
reports the same ceiling of `11-12` rather than implying precision the
classification does not support.

---

## 5. The 5-8 frame override, honestly

The `240 / N` rule maps a 5-8 frame window to 48, 40, 34.3 and 30 FPS. The
exception engine relabels it `60 FPS` when all of these hold:

1. the raw window is in `[5, 8]`
2. `timeWarp == 1.0` exactly
3. playback speed is Normal (the accelerated driver perturbs the cadence)
4. `tick % 4 == 0` — the press landed on the 60 Hz tick grid
5. the nearest boundary was a **ground or slope** transition
6. the player is alive, unpaused, and grounded or on a slope

### Why conditions 2-6 are necessary

**A true 60 Hz frame-perfect window on a 240 TPS baseline is 4 ticks, not 5-8.**
`240 / 60 = 4`. So "this click is really a 60 FPS window" cannot be concluded from
the window size — it has to come from external state. That is exactly what the
conditions above read.

Condition 4 is the load-bearing one. On a 240 TPS baseline, ticks 0, 4, 8, 12… are
the instants a 60 Hz system samples, so a press on one of those ticks is genuinely
resolved by the 60 Hz cadence rather than by the finer one. Condition 5 excludes
orb and input-state windows, which are resolved by the player's own physics rather
than by any external cadence.

### What it does not do

The override changes **only the drawn string**. The click stays in its measured
bucket, so `Frame Window: Count` keeps reporting what actually happened. A tally
that counted relabelled clicks as 4-frame windows would be lying about the run.

### What it is

A display heuristic keyed off the state signals above, guarded so it fires rarely
and never when the clock is perturbed. It is labelled as such in the code, and the
**60 FPS Window Override** setting turns it off entirely.