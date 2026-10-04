# Changelog

## v1.0.0

Initial release.

### Pause menu controls

- **FP** button on the pause menu opens the control popup.
- **Frames** button inside the popup opens the per-click list.
- Speed grid of 18 presets: 1x 2x 3x 4x 6x 8x 12x 16x 24x 32x 48x 64x 73x 96x
  128x 150x 183x 200x. The selected preset is drawn at full opacity.
- The status line reports the **requested** and **achieved** rate separately,
  because the per-frame tick ceiling caps what is reachable: at 60 FPS rendering
  the ceiling allows 16 x 60 = 960 ticks per real second, i.e. 4x the 240 TPS
  baseline. Anything above 4x is requested but not reachable, so a 73x request
  reads `73.0x req / 3.9x act`.
- Click offset in whole ticks, +/-12, nudged from the popup and clamped to the
  window search radius. Applied to injected playback clicks only; the recorder
  never shifts, so a capture always records what actually happened.

### Frame window overrides

- Per-click log of every classified click, 1024 entries, cleared per attempt.
  Previously a click was counted into a bucket and discarded, so there was
  nothing to display or correct.
- The FRAMES list shows one row per click: sequence number, the window the
  tracker measured, its FPS ceiling label, and a text field to override it.
- An override re-evaluates against the state captured **at the click**, not the
  state at the moment of typing, so the 60 FPS override still sees the original
  tick, time warp and boundary kind.
- The original measurement is always shown next to a corrected one (`measured N`)
  so an override can never hide the evidence it replaces.
- Overrides feed back into the aggregate tallies, which are rebuilt from the log
  rather than decremented, so the corner counters cannot drift from the list.
- One shared text field for the whole list rather than one per row: a full attempt
  can hold 1024 clicks, and 1024 live cocos2d text nodes with their own touch
  delegates would cost more frame time than the physics they are measuring.

### Ingestion

- Decoders for Silicate binary, XDBot text, xBot 2.1 text, Mega Hack replay
  JSON, Eclipse CSV, and a plain `fps / frame hold p2` text dialect.
- Content sniffing rather than extension matching, so a mislabelled file still
  imports.
- Strict 240 FPS gate with distinct rejection reasons (`FpsMissing`,
  `FpsMismatch`, `FpsNotFinite`, `NotFrameBased`, `TooManyInputs`) and no
  "closest match" path. Toggleable via **Strict 240 FPS Gate**.
- xBot `pro_plus` files are rejected: they store X positions, so their tick rate
  cannot be verified at all.
- Depth-limited JSON reader with element and file-size ceilings, so a hostile
  file cannot exhaust the stack or the heap.

### Recording

- Raw press/release capture at `GJBaseGameLayer::handleButton`, the engine's
  single input entry point. One predicated branch per input event, no polling.
- Edge collapse: an identical event repeated on the next frame stretches the
  previous edge instead of adding a redundant row.
- Writers for Silicate, XDBot, Mega Hack JSON, Eclipse and plain text.

### Playback

- Forward-only cursor over a pre-sorted input vector: O(1) amortised per tick,
  no allocation, exact input ordering including multiple inputs on one frame.
- Input is injected through `handleButton`, so the engine's own input
  bookkeeping applies.
- Desync latch instead of silent skipping. The latch is anchored to the engine
  tick index and trips if playback observes the same tick twice *or* misses one,
  and the trip is surfaced once per run through an `FLAlertLayer` — a macro that
  stops quietly is indistinguishable from the mod hanging.
- Opt-in accelerated timescale: extra `PlayLayer::update` calls per rendered
  frame, hard-capped at 16, reentrancy-guarded, with the fixed clock keeping any
  un-drained remainder so capping delays work rather than discarding it. The
  driven ticks run *after* the frame's own tick closes and are bracketed by the
  same pre/post pair, so a tick's physics step always sits between that tick's
  own hooks and the tick index stays a single source of truth.

### Analysis

- Interaction-boundary tracker over ground flags, slope contact, ground/slope
  object ids, gravity, ring contact count, sliding, rotating, dead and jump
  buffered. 1024-entry ring buffer, no allocation.
- Boundary dots are drained per *boundary*, not per rendered frame: in
  accelerated mode several boundaries can occur inside one frame, and the exact
  position of each is recorded in the ring. Death is classified as an
  input-state transition rather than a ground one, so it cannot trigger the
  override.
- Window buckets `1`, `2`, `3`, `4`, `5-6`, `7-8`, `9-10`, `11-12`.
- Frame-rate ceiling `240 / window`, exact integers kept exact, everything else
  truncated to one decimal, computed through a divide that cannot produce inf
  or NaN.
- Exception engine that relabels a 5-8 frame window `60 FPS` when the click was
  on the 60 Hz tick grid with a surface-anchored boundary, vanilla time warp,
  normal playback speed and a live player. Bucket is deliberately unchanged.
- Auto-load scans `.macro`, `.json`, `.txt`, `.slc` and `.csv` (case-insensitively),
  so a Silicate or Eclipse recording saved by this mod can be loaded back. The
  format still comes from content sniffing, not the extension.

### Interface

- One batched `CCDrawNode` for circles, boundary dots and the statistics panel;
  a fixed 48-slot pooled label set for the per-click strings; one label for the
  corner tally with a dirty check so a steady frame uploads no text.
- Every primitive is limited to APIs whose published signature is unambiguous:
  `CCDrawNode::drawDot` for geometry (the click "circle" is a filled disc plus a
  24-dot ring) and `CCLabelBMFont::create(str, fnt)` plus `setScale` for text.
  The panel rect is measured from `getContentSize() * getScale()` and cached
  until the tally text changes.
- Circles and labels live in world space so they track the camera; the tally
  lives in screen space, repositioned each frame from the director's win size.
- Markers fade over the last 40% of their lifetime instead of popping.

### Known limitations

See `README.md` and `docs/FORMATS.md`. The window figure is an
interaction-boundary proxy rather than a full physics roll-forward, the
accelerated timescale is experimental, editor sessions are not covered, and
Eclipse's schema is implemented from community documentation rather than from the
original author's source.
