# AutoFPCount

Frame-window analyser and macro recorder/playback overlay for Geometry Dash 2.2081.

AutoFPCount imports macros recorded by **Silicate**, **XDBot**, **Mega Hack**,
**Eclipse** and **GDR** (`.gdr` / `.gdr2`), refuses any file whose declared tick
rate is not exactly 240 FPS, replays the accepted macro, and for every press draws
the exact grid position, the ticks of slack it had, and the frame-rate ceiling that
slack corresponds to.

```
1 frame    -> 240 FPS
2 frames   -> 120 FPS
3 frames   ->  80 FPS
4 frames   ->  60 FPS
5-6 frames ->  48 / 40 FPS
7-8 frames ->  34.3 / 30 FPS
9-10       ->  26.7 / 24 FPS
11-12      ->  21.8 / 20 FPS
```

A live `Frame Window: Count` list sits in the screen corner and increments the
instant each click is classified.

---

## Requirements

| | |
|---|---|
| Geometry Dash | 2.2081 |
| Loader | Geode 5.x (`GEODE_SDK` must be set) |
| Toolchain | CMake 3.21+, a C++23 compiler |

## Building

```powershell
$env:GEODE_SDK = "C:\path\to\geode-sdk"
cmake -B build -T v143 -A x64
cmake --build build --config RelWithDebInfo
```

The built mod lands in `build\RelWithDebInfo\` as `AutoFPCount.dll`. Copy the
whole output directory into your Geode mods folder:

```
%APPDATA%\Geode\mods\novabot.autofpcount\
    mod.json
    AutoFPCount.dll
```

Then add `AutoFPCount.dll` to `geode.loader` in the Geode launcher.

---

## Using it

1. Start a level in **practice mode**.
2. The mod ingests a macro automatically on the first frame of the attempt.
3. Every click gets a circle with its frame-rate ceiling beside it, and the
   corner list tallies the windows.

### Supplying a macro

Put macro files in your mod save directory:

```
%APPDATA%\Geode\saved\novabot.autofpcount\macros\active.macro
```

`active.macro` is preferred. If it is absent, the first file (in sorted order)
that passes the 240 FPS gate is used. Rejections are reported in the Geode log
and as an on-screen alert naming the exact reason.

### Recording

With the **Enable Recorder** setting on, every press and release during a live
attempt is captured as a native 240 FPS structure. Export it with:

```cpp
std::string error;
afpc::GameSession::get().exportRecording(
    afpc::Recorder::defaultExportPath(),
    afpc::MacroFormat::Silicate,   // or Unknown to infer from the extension
    &error);
```

Writers exist for Silicate binary (`.slc`), Mega Hack JSON (`.json`), Eclipse CSV
(`.csv`), plain text (`.txt`) and XDBot text. `.macro` is genuinely ambiguous —
Silicate uses it for a binary container and the text tools use it too — so it
defaults to XDBot text; pass a concrete `MacroFormat` to override.

---

## Settings

| Setting | Default | Effect |
|---|---|---|
| Enable Overlay | on | Circles, labels and the corner statistics list |
| Enable Recorder | on | Capture live input for export |
| Strict 240 FPS Gate | on | Reject any file not declared at 240 FPS |
| 60 FPS Window Override | on | Edge-case relabel, see below |
| Accelerated Timescale | **off** | Extra logic ticks per rendered frame |
| Show Interaction Boundaries | on | Dots on the ticks windows are measured against |
| Auto Start Playback | on | Begin replaying at the start of each attempt |

---

## How the numbers are produced

### Window size

A click's window is the number of ticks of slack before the press stops landing.
AutoFPCount computes it as the distance, in ticks, from the click to the nearest
**interaction boundary** — a tick where the player's collision or interaction
state changed. Those are the only ticks where one extra or missing tick of input
can change what the player hits.

Sampled once per tick, all plain member loads:

* four of the engine's ground flags
* slope contact and which object the slope is
* the ground object id
* gravity direction
* ring/orb contact counts
* sliding, rotating, dead, jump buffered

**This is a proxy, and it is a good one, but it is not a full physics
roll-forward.** Deriving an exact window means re-simulating the player with and
without the click and comparing outcomes, which is not affordable during live
play. See `docs/FORMATS.md` for the precise limitations.

### Frame-rate ceiling

`ceiling = 240 / window`. Exact integers stay exact (`240 FPS`, `60 FPS`);
everything else is truncated to one decimal (`34.3 FPS`). Truncation is
deliberate — a ceiling is never rounded *up* past the next bucket.

### The 5-8 frame override

By the `240 / N` rule a 5-8 frame window reads 48, 40, 34.3 or 30 FPS. The
exception engine relabels it **"60 FPS"** when all of the following hold:

1. the raw window is in `[5, 8]`
2. time warp is exactly `1.0` — the run is on the vanilla clock
3. playback speed is Normal — the accelerated driver perturbs the cadence
4. the click landed on the 60 Hz tick grid, i.e. `tick % 4 == 0`
5. the nearest boundary was a **ground or slope** transition
6. the player is alive, unpaused, and grounded or on a slope

Note what this does **not** do: it does not move the click into the `4` bucket.
Only the drawn string changes. The `Frame Window: Count` list keeps reporting the
measured window, so the statistics stay truthful while the label is overridden.

> **On the arithmetic.** A true 60 Hz frame-perfect window on a 240 TPS baseline
> is **4 ticks** (`240 / 60 = 4`), not 5-8. The override therefore cannot be
> derived from the window size — it has to come from external state, which is why
> conditions 2-6 exist. It is a display heuristic, and the code says so.

---

## Design notes

**No threads, no locks.** The playback engine, recorder, tracker and overlay all
run on the game's main thread inside `PlayLayer::update`. There are no threads,
no mutexes and no atomics anywhere in the mod, so lock contention and thread
starvation are not "unlikely" — they are structurally impossible.

**No dropped steps.** Playback keeps a forward-only cursor over a pre-sorted
input vector, so a tick costs O(1) amortised and allocates nothing. When the
accelerated driver's per-frame cap is reached the remainder is *kept* in an
integer nanosecond accumulator and simulated next frame. Playback never advances
past a tick that was not actually simulated; it latches a desync flag instead.

**No float drift in the tick clock.** At 240 TPS a tick is 4166666.66… ns, which
is not representable in binary floating point. `FixedClock` accumulates in
`int64` nanoseconds, which makes the long-run tick count exact and the sub-tick
phase a pure remainder with zero drift.

**No division that can produce NaN or infinity.** There is exactly one division
in the maths engine (`240 / window`) and it goes through `safeDivide`, which
returns `0.0` rather than `inf` for a zero or non-finite denominator. Every
conversion from gameplay state runs through `saturatingInt`, which clamps
non-finite and out-of-range input instead of letting it reach the bucket table.

**One tick index, one owner.** `GameSession::m_tick` is incremented in exactly one
place, and every simulated tick is bracketed by `preEngineTick` / `postEngineTick`.
The accelerated driver is not an exception to that: it calls `PlayLayer::update`
extra times *after* the frame's own tick has closed, bracketing each driven tick
with the same pair, so driven ticks are indistinguishable from real ones. This is
why the driver must not run between `preEngineTick` and `PlayLayer::update` — a
physics step for tick N has to sit between tick N's own two hooks, or the inputs
injected for tick N would be consumed by a different tick's step.

**Desync is surfaced, not swallowed.** If the tick stream ever diverges from what
playback observed, playback latches a desync, stops rather than skipping a
recorded step, and the mod shows an `FLAlertLayer` explaining why. A macro that
quietly stops mid-run looks like the mod hanging, so the failure is reported
exactly once per run.

**No allocation after init.** Overlay labels come from a fixed 48-slot pool,
circles share one batched `CCDrawNode`, pending clicks use a fixed 8-slot array,
and the statistics string is rebuilt into a reused buffer only when a counter
actually changed.

**Bounded ingestion.** File size is capped at 64 MiB and decoded input count at
8 million, both independent of each other, so a header that declares a huge count
on a short file cannot drive a large `reserve()`.

---

## Project layout

```
CMakeLists.txt
mod.json
about.md
docs/FORMATS.md              format schemas, research provenance, limitations
tools/
  make_gdr_fixtures.py       emits byte-exact .gdr / .gdr2 test vectors
  check_gdr_reader.py        runs them through a mirror of the C++ reader
src/
  main.cpp                    all $modify hooks + accelerated driver
  core/
    Constants.hpp             baseline constants and bounds
    Numeric.hpp               fp guards, safeDivide, FixedClock, label formatting
  macro/
    MacroFormat.hpp/.cpp      sniffing, 6 parsers, 5 writers, the 240 FPS gate
    GdrReader.hpp/.cpp        GDR v1 (.gdr) and v2 (.gdr2), no dependencies
    JsonLite.hpp/.cpp         depth-limited JSON + MessagePack readers
  playback/
    PlaybackEngine.hpp/.cpp   forward-only cursor, dual speed, desync latch
    Recorder.hpp/.cpp         native 240 FPS capture + export
  analysis/
    InteractionTracker.hpp/.cpp  interaction-boundary ring buffer
    WindowAnalyzer.hpp/.cpp   buckets, 240/N ceiling, exception engine
    FrameStats.hpp/.cpp       the "Frame Window: Count" tally
  ui/
    Overlay.hpp/.cpp          cocos2d-x circles, labels, corner panel
  game/
    GameSession.hpp/.cpp      subsystem owner, attempt lifecycle, ingestion
```

## Honest caveats

* **Not compiled here.** No Geode SDK or compiler was available in the authoring
  environment, so this has not been built. Every external API used was checked
  against `docs.geode-sdk.org`, and the overlay deliberately sticks to the one
  primitive whose published signature is unambiguous (`CCDrawNode::drawDot`) plus
  `CCLabelBMFont::create(str, fnt)`. Still: expect to fix an include path or a
  signature on the first build.
* **Editor sessions are not covered.** The hooks are on `PlayLayer`, so playing a
  level in the editor does not run the analysis.
* **Accelerated timescale is experimental.** It re-enters `PlayLayer::update`, which
  is genuinely risky around cocos2d actions and schedulers. It is off by default,
  hard-bounded, and reentrancy-guarded — but it has not been stress tested.
* **The window is a proxy**, as described above.
* **Eclipse's own source is not public.** Its dialect is implemented from the
  widely documented `fps / count / x, y, delay, down` layout; see
  `docs/FORMATS.md` for what is verified versus inferred.