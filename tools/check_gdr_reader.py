#!/usr/bin/env python3
"""
Executable mirror of GdrReader.cpp, used to validate the decoder's algorithm
before it is compiled.

There is no local C++ toolchain in this environment and no Geometry Dash
install to test against, so the format logic is checked here instead: this is a
line-for-line port of the C++ reader, and it is run against

  * the fixtures produced by make_gdr_fixtures.py, and
  * a real .gdr capture published in maxnut/GDR-converter (test/test.gdr).

Passing both means the *algorithm* is right. It does not prove the C++ compiles;
CI does that, by building the mod and asserting the same contracts.

A divergence between this port and the C++ is a bug in whichever one is wrong,
so keep them edited together.
"""

import json
import os
import struct
import sys

K_MAGIC = b"GDR"
K_MAX_VARINT_BYTES = 10
K_MAX_ELEMENTS = 4_000_000
K_MAX_DEPTH = 32


class Truncated(Exception):
    pass


class Bad(Exception):
    pass


# ---------------------------------------------------------------------------
# v2 cursor - mirrors class Cursor in GdrReader.cpp
# ---------------------------------------------------------------------------

class Cursor:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0
        self.error = None

    def remaining(self):
        return len(self.data) - self.pos

    def empty(self):
        return self.pos >= len(self.data)

    def raw(self, n):
        if n > self.remaining():
            self.error = "truncated"
            raise Truncated("raw")
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def skip(self, n):
        if n > self.remaining():
            self.error = "truncated"
            raise Truncated("skip")
        self.pos += n

    def varint(self):
        value = 0
        shift = 0
        for _ in range(K_MAX_VARINT_BYTES):
            if self.empty():
                self.error = "truncated varint"
                raise Truncated("varint")
            b = self.data[self.pos]
            self.pos += 1
            payload = b & 0x7F
            if shift >= 64 or (shift == 63 and payload > 1):
                self.error = "varint overflows 64 bits"
                raise Bad("varint overflow")
            value |= payload << shift
            if (b & 0x80) == 0:
                return value
            shift += 7
        self.error = "varint longer than 10 bytes"
        raise Bad("varint too long")

    def varint_u32(self):
        v = self.varint()
        if v > 0xFFFFFFFF:
            self.error = "value does not fit in 32 bits"
            raise Bad("u32 overflow")
        return v

    def varint_int(self):
        v = self.varint()
        if v > 0x7FFFFFFF:
            self.error = "value does not fit in a signed int"
            raise Bad("int overflow")
        return v

    def boolean(self):
        return self.varint() != 0

    def string(self):
        length = self.varint()
        if length > self.remaining():
            self.error = "string length exceeds remaining buffer"
            raise Bad("string length")
        out = self.data[self.pos:self.pos + length]
        self.pos += length
        return out.decode("utf-8", "replace")

    def be32(self):
        return struct.unpack(">f", self.raw(4))[0]

    def be64(self):
        return struct.unpack(">d", self.raw(8))[0]


# ---------------------------------------------------------------------------
# MessagePack - mirrors class MsgPackParser in JsonLite.cpp
# ---------------------------------------------------------------------------

class MsgPack:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

    def remaining(self):
        return len(self.data) - self.pos

    def empty(self):
        return self.pos >= len(self.data)

    def byte(self):
        if self.empty():
            raise Truncated("msgpack byte")
        b = self.data[self.pos]
        self.pos += 1
        return b

    def be(self, width):
        if width > self.remaining():
            raise Truncated("msgpack be")
        out = int.from_bytes(self.data[self.pos:self.pos + width], "big")
        self.pos += width
        return out

    def reserve_ok(self, count, min_bytes):
        if count * min_bytes > self.remaining():
            raise Bad("declared length exceeds remaining buffer")
        if count > K_MAX_ELEMENTS:
            raise Bad("element count exceeds the safety ceiling")

    def take_string(self, length):
        self.reserve_ok(length, 1)
        out = self.data[self.pos:self.pos + length].decode("utf-8", "replace")
        self.pos += length
        return out

    def skip_payload(self, length):
        if length > self.remaining():
            raise Truncated("msgpack payload")
        self.pos += length

    def parse_map(self, pairs, depth):
        self.reserve_ok(pairs, 2)
        out = {}
        for _ in range(pairs):
            key = self.parse_key()
            out[key] = self.parse_value(depth + 1)
        return out

    def parse_array(self, count, depth):
        self.reserve_ok(count, 1)
        return [self.parse_value(depth + 1) for _ in range(count)]

    def parse_key(self):
        if self.empty():
            raise Truncated("msgpack key")
        tag = self.data[self.pos]
        if tag & 0xE0 == 0xA0:
            self.pos += 1
            return self.take_string(tag & 0x1F)
        if tag in (0xD9, 0xDA, 0xDB):
            self.pos += 1
            width = {0xD9: 1, 0xDA: 2, 0xDB: 4}[tag]
            return self.take_string(self.be(width))
        self.parse_value(1)  # non-string key: skipped
        return ""

    def parse_value(self, depth):
        if depth > K_MAX_DEPTH:
            raise Bad("maximum nesting depth exceeded")
        if self.empty():
            raise Truncated("msgpack value")

        tag = self.byte()

        if tag <= 0x7F:
            return float(tag)
        if tag >= 0xE0:
            return float(tag - 256)
        if tag & 0xF0 == 0x80:
            return self.parse_map(tag & 0x0F, depth)
        if tag & 0xF0 == 0x90:
            return self.parse_array(tag & 0x0F, depth)
        if tag & 0xE0 == 0xA0:
            return self.take_string(tag & 0x1F)

        if tag == 0xC0:
            return None
        if tag == 0xC2:
            return False
        if tag == 0xC3:
            return True

        if tag == 0xCC: return float(self.be(1))
        if tag == 0xCD: return float(self.be(2))
        if tag == 0xCE: return float(self.be(4))
        if tag == 0xCF: return float(self.be(8))
        if tag == 0xD0: return float(struct.unpack(">b", self.be(1).to_bytes(1, "big"))[0])
        if tag == 0xD1: return float(struct.unpack(">h", self.be(2).to_bytes(2, "big"))[0])
        if tag == 0xD2: return float(struct.unpack(">i", self.be(4).to_bytes(4, "big"))[0])
        if tag == 0xD3: return float(struct.unpack(">q", self.be(8).to_bytes(8, "big"))[0])

        if tag == 0xCA:
            v = struct.unpack(">f", self.be(4).to_bytes(4, "big"))[0]
            return v if v == v and abs(v) != float("inf") else 0.0
        if tag == 0xCB:
            v = struct.unpack(">d", self.be(8).to_bytes(8, "big"))[0]
            return v if v == v and abs(v) != float("inf") else 0.0

        if tag in (0xD9, 0xDA, 0xDB):
            width = {0xD9: 1, 0xDA: 2, 0xDB: 4}[tag]
            return self.take_string(self.be(width))

        if tag == 0xDC: return self.parse_array(self.be(2), depth)
        if tag == 0xDD: return self.parse_array(self.be(4), depth)
        if tag == 0xDE: return self.parse_map(self.be(2), depth)
        if tag == 0xDF: return self.parse_map(self.be(4), depth)

        if tag == 0xC4: self.skip_payload(self.be(1)); return None
        if tag == 0xC5: self.skip_payload(self.be(2)); return None
        if tag == 0xC6: self.skip_payload(self.be(4)); return None
        if tag == 0xC7: self.skip_payload(self.be(1) + 1); return None
        if tag == 0xC8: self.skip_payload(self.be(2) + 1); return None
        if tag == 0xC9: self.skip_payload(self.be(4) + 1); return None
        if tag == 0xD4: self.skip_payload(2); return None
        if tag == 0xD5: self.skip_payload(3); return None
        if tag == 0xD6: self.skip_payload(5); return None
        if tag == 0xD7: self.skip_payload(9); return None
        if tag == 0xD8: self.skip_payload(17); return None

        raise Bad("unknown MessagePack type tag 0x%02x" % tag)


# ---------------------------------------------------------------------------
# Replay decode - mirrors afpc::gdr::parse
# ---------------------------------------------------------------------------

class Replay:
    def __init__(self):
        self.version = 0
        self.author = ""
        self.description = ""
        self.bot_name = ""
        self.bot_version = ""
        self.level_id = 0
        self.level_name = ""
        self.framerate = 0.0
        self.platformer = False
        self.deaths = []
        self.inputs = []  # list of (frame, button, player2, down)


def looks_like_binary(data: bytes) -> bool:
    return len(data) >= 3 and data[:3] == K_MAGIC


def looks_like_json_document(text: str) -> bool:
    i = 0
    while i < len(text) and text[i] in " \t\n\r":
        i += 1
    if i >= len(text) or text[i] != "{":
        return False
    head = text[:4096]
    return any(k in head for k in ('"inputs"', '"bot"', '"gameVersion"'))


def _copy_str(node, key, out, attr):
    v = node.get(key)
    if isinstance(v, str):
        setattr(out, attr, v)


def _copy_num(node, key, attr):
    v = node.get(key)
    if isinstance(v, (int, float)):
        setattr(out, attr, float(v))


def parse_binary(data, replay, max_inputs):
    """Field order matches gdr.hpp's importData() exactly."""
    cur = Cursor(data)

    if cur.raw(3) != K_MAGIC:
        raise Bad("not a GDR v2 stream (missing 'GDR' magic)")

    replay.version = cur.varint_int()
    input_tag = cur.string()
    replay.author = cur.string()
    replay.description = cur.string()
    cur.be32()                      # duration, informational only
    replay.game_version = cur.varint_int()
    replay.framerate = cur.be64()
    replay.seed = cur.varint_int()
    replay.coins = cur.varint_int()
    replay.ldm = cur.boolean()
    replay.platformer = cur.boolean()
    replay.bot_name = cur.string()
    replay.bot_version = cur.varint_int()
    replay.level_id = cur.varint_u32()
    replay.level_name = cur.string()

    cur.skip(cur.varint())          # opaque extension block

    death_count = cur.varint()
    if death_count > cur.remaining() + 1:
        raise Bad("death count exceeds the remaining buffer")
    previous = 0
    for _ in range(death_count):
        previous += cur.varint()
        replay.deaths.append(previous)

    input_count = cur.varint()
    if input_count > max_inputs:
        raise Bad("input count exceeds the safety ceiling")

    p1_inputs = cur.varint()
    has_input_extension = bool(input_tag)
    previous_frame = 0

    # Reads until exhausted rather than trusting input_count, as upstream does.
    while not cur.empty():
        packed = cur.varint()
        if replay.platformer:
            delta, button, down = packed >> 3, (packed >> 1) & 3, bool(packed & 1)
        else:
            delta, button, down = packed >> 1, 1, bool(packed & 1)

        frame = previous_frame + delta
        if frame > 0xFFFFFFFF:
            raise Bad("input frame exceeds 32-bit range")

        replay.inputs.append((frame, button, p1_inputs == 0, down))

        if has_input_extension:
            cur.skip(cur.varint())

        previous_frame = frame
        if p1_inputs > 0:
            p1_inputs -= 1
            if p1_inputs == 0:
                previous_frame = 0

    # The declared count must be satisfied exactly; otherwise a truncated stream
    # decodes into a quietly shorter replay.
    if len(replay.inputs) != input_count:
        raise Bad("declares %d inputs but only %d could be decoded"
                  % (input_count, len(replay.inputs)))

    replay.inputs.sort(key=lambda i: i[0])   # stable, as upstream does

    if replay.version != 2:
        raise Bad("GDR version %d in a binary stream is not supported" % replay.version)


def parse_document(root, replay, max_inputs):
    """v1 extraction, mirroring parseDocument() in GdrReader.cpp."""
    if not isinstance(root, dict):
        raise Bad("GDR v1 root is not an object")

    def num(key, attr=None):
        v = root.get(key)
        if isinstance(v, (int, float)):
            if attr:
                setattr(replay, attr, float(v))
            return float(v)
        return None

    v = root.get("version")
    replay.version = int(v) if isinstance(v, (int, float)) else 0
    _copy_str(root, "author", replay, "author")
    _copy_str(root, "description", replay, "description")
    for key in ("gameVersion", "seed", "coins"):
        got = num(key)
        if got is not None:
            setattr(replay, key.lower(), int(got))
    num("duration", "duration")
    num("framerate", "framerate")
    if isinstance(root.get("ldm"), bool):
        replay.ldm = root["ldm"]
    if isinstance(root.get("platformer"), bool):
        replay.platformer = root["platformer"]

    bot = root.get("bot")
    if isinstance(bot, dict):
        _copy_str(bot, "name", replay, "bot_name")
        bv = bot.get("version")
        if isinstance(bv, str):
            replay.bot_version = bv
        elif isinstance(bv, (int, float)):
            replay.bot_version = str(int(bv))

    level = root.get("level")
    if isinstance(level, dict):
        lid = level.get("id")
        if isinstance(lid, (int, float)) and lid >= 0:
            replay.level_id = int(min(lid, 4294967295))
        _copy_str(level, "name", replay, "level_name")

    inputs = root.get("inputs")
    if not isinstance(inputs, list):
        raise Bad("GDR v1 document has no 'inputs' array")
    if len(inputs) > max_inputs:
        raise Bad("GDR v1 input count exceeds the safety ceiling")

    for i, ev in enumerate(inputs):
        if not isinstance(ev, dict):
            raise Bad("GDR v1 input %d is not an object" % i)
        frame = ev.get("frame")
        if not isinstance(frame, (int, float)) or frame < 0 or frame > 4294967295:
            raise Bad("GDR v1 input %d has an out-of-range frame" % i)
        btn = ev.get("btn")
        btn = int(min(max(btn, 0), 3)) if isinstance(btn, (int, float)) else 1
        down = ev.get("down")
        p2 = ev.get("2p")
        replay.inputs.append((
            int(frame),
            btn,
            bool(p2) if isinstance(p2, bool) else False,
            bool(down) if isinstance(down, bool) else False,
        ))

    replay.inputs.sort(key=lambda i: i[0])


def parse(data, max_inputs=8_000_000):
    """Mirrors afpc::gdr::parse."""
    if not data:
        raise Bad("empty buffer")

    replay = Replay()
    if looks_like_binary(data):
        parse_binary(data, replay, max_inputs)
        return replay

    if data[0:1] == b"{":
        try:
            root = json.loads(data.decode("utf-8"))
        except Exception as exc:
            raise Bad("GDR v1 document could not be decoded: %s" % exc)
    else:
        mp = MsgPack(data)
        try:
            root = mp.parse_value(0)
            if not mp.empty():
                raise Bad("trailing bytes after root value")
        except (Truncated, Bad) as exc:
            raise Bad("GDR v1 document could not be decoded: %s" % exc)

    parse_document(root, replay, max_inputs)

    if replay.version != 1:
        raise Bad("GDR version %d in a v1 document is not supported" % replay.version)
    return replay


# ---------------------------------------------------------------------------
# Fixtures + the real published capture
# ---------------------------------------------------------------------------

EXPECTED = {
    "simple.gdr2":      ([0, 12, 30], 240.0),
    "release.gdr2":     ([0, 6, 12, 18], 240.0),
    "platformer.gdr2":  ([0, 5, 9], 240.0),
    "bigdelta.gdr2":    ([0, 5000, 100000], 240.0),
    "twoplayer.gdr2":   ([0, 4, 9, 10], 240.0),
    "inputext.gdr2":    ([0, 8], 240.0),
    "deaths.gdr2":      ([0, 60, 120], 240.0),
    "fps60.gdr2":       ([0, 12, 30], 60.0),
    "empty.gdr2":       ([], 240.0),
    "simple.gdr":       ([0, 12, 30], 240.0),
    "noframerate.gdr":  ([0, 12, 30], 240.0),
    "twoplayer.gdr":    ([0, 4, 9, 10], 240.0),
    "fps60.gdr":        ([0, 12, 30], 60.0),
}

REJECT = ["truncated.gdr2", "badmagic.gdr2", "notgdr.bin"]


def check_fixtures(directory):
    failures = []

    for name, (want_ticks, want_fps) in sorted(EXPECTED.items()):
        path = os.path.join(directory, name)
        if not os.path.exists(path):
            failures.append("%s: fixture missing" % name)
            continue
        with open(path, "rb") as handle:
            data = handle.read()
        try:
            replay = parse(data)
        except (Truncated, Bad) as exc:
            failures.append("%s: rejected but should decode (%s)" % (name, exc))
            continue

        got_ticks = [i[0] for i in replay.inputs]
        if got_ticks != want_ticks:
            failures.append("%s: ticks %s != expected %s" % (name, got_ticks, want_ticks))
        # v1 falls back to the format default when `framerate` is absent.
        got_fps = replay.framerate if replay.framerate > 0 else (
            240.0 if replay.version == 1 else 0.0)
        if abs(got_fps - want_fps) > 1e-9:
            failures.append("%s: fps %s != expected %s" % (name, got_fps, want_fps))
        print("  ok  %-18s v%d  %2d input(s)  %.0f tps" %
              (name, replay.version, len(replay.inputs), got_fps))

    for name in REJECT:
        path = os.path.join(directory, name)
        if not os.path.exists(path):
            failures.append("%s: fixture missing" % name)
            continue
        with open(path, "rb") as handle:
            data = handle.read()
        try:
            parse(data)
            failures.append("%s: decoded but should have been rejected" % name)
        except (Truncated, Bad) as exc:
            print("  ok  %-18s rejected: %s" % (name, exc))

    return failures


def check_real_capture(path):
    """maxnut/GDR-converter ships a real .gdr. If the MessagePack reader is wrong
    this is what exposes it, since it was written by nlohmann rather than by me."""
    if not os.path.exists(path):
        print("  --  real capture not present, skipping")
        return []

    with open(path, "rb") as handle:
        data = handle.read()

    try:
        replay = parse(data)
    except (Truncated, Bad) as exc:
        return ["real capture failed to decode: %s" % exc]

    if replay.version != 1:
        return ["real capture: expected version 1, got %d" % replay.version]
    if not replay.inputs:
        return ["real capture: decoded zero inputs"]
    if replay.framerate <= 0:
        return ["real capture: framerate missing"]

    # Frames must be strictly ordered after the stable sort and non-negative.
    frames = [i[0] for i in replay.inputs]
    if frames != sorted(frames):
        return ["real capture: frames out of order"]
    if frames[0] < 0:
        return ["real capture: negative frame"]

    print("  ok  real .gdr         v%d  %d input(s)  %.0f tps  author=%r bot=%r" %
          (replay.version, len(replay.inputs), replay.framerate,
           replay.author, replay.bot_name))
    return []


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: check_gdr_reader.py <fixtures-dir> [real-capture.gdr]", file=sys.stderr)
        return 2

    print("fixtures:")
    failures = check_fixtures(sys.argv[1])
    print("\nreal-world capture:")
    failures += check_real_capture(sys.argv[2] if len(sys.argv) > 2 else "")

    print()
    if failures:
        for line in failures:
            print("  FAIL  " + line)
        return 1

    print("all GDR reader checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
