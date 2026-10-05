#!/usr/bin/env python3
"""
Generates GDR test fixtures for AutoFPCount's importer.

The encoder here is written from maxnut/GDReplayFormat's *writer* (branch
`gdr2`, include/gdr/gdr.hpp) so the fixtures are byte-exact reproductions of
what a real recorder emits, not an approximation. CI then decodes them with
GdrReader and asserts the expected inputs, which is the only way to catch a
misreading of the format without a Geometry Dash install to hand.

Layout of a v2 (.gdr2) stream, in order:

    "GDR"                 3 raw bytes, written via the char[N] overload
    version               varint
    inputTag              string, length-prefixed
    author                string
    description           string
    duration              f32 big endian
    gameVersion           varint
    framerate             f64 big endian
    seed                  varint
    coins                 varint
    ldm                   varint bool
    platformer            varint bool
    bot.name              string
    bot.version           varint
    level.id              varint
    level.name            string
    extensionSize         varint, then that many opaque bytes
    deathCount            varint, then that many accumulating deltas
    inputCount            varint
    p1InputCount          varint
    inputs, player 1 then player 2:
        packed            varint, delta << 3 | button << 1 | down  (platformer)
                          varint, delta << 1 | down                (classic)
        [per-input extension, only when inputTag is non-empty]

v1 (.gdr) is a serialised JSON document; the canonical encoding is MessagePack,
which this script emits by hand.

Usage:  python tools/make_gdr_fixtures.py <output-dir>
"""

import os
import struct
import sys


# ---------------------------------------------------------------------------
# Encoding primitives
# ---------------------------------------------------------------------------

def varint(value: int) -> bytes:
    """LEB128, matching binarystream.hpp's integral getBytes/consume."""
    if value < 0:
        raise ValueError("varint is unsigned")
    out = bytearray()
    if value == 0:
        return bytes([0])
    while value > 0:
        byte = value & 0x7F
        value >>= 7
        if value > 0:
            byte |= 0x80
        out.append(byte)
    return bytes(out)


def string(text: str) -> bytes:
    """Length-prefixed. Note this is NOT NUL terminated, contrary to the
    upstream readme; binarystream.hpp is authoritative."""
    raw = text.encode("utf-8")
    return varint(len(raw)) + raw


def be32(value: float) -> bytes:
    return struct.pack(">f", value)


def be64(value: float) -> bytes:
    return struct.pack(">d", value)


# ---- MessagePack (only the subset GDR v1 needs) ---------------------------

def mp_uint(value: int) -> bytes:
    if value < 0x80:
        return bytes([value])
    if value <= 0xFF:
        return b"\xcc" + bytes([value])
    if value <= 0xFFFF:
        return b"\xcd" + struct.pack(">H", value)
    if value <= 0xFFFFFFFF:
        return b"\xce" + struct.pack(">I", value)
    return b"\xcf" + struct.pack(">Q", value)


def mp_int(value: int) -> bytes:
    if value >= 0:
        return mp_uint(value)
    if value >= -32:
        return bytes([value & 0xFF])
    if value >= -128:
        return b"\xd0" + struct.pack(">b", value)
    if value >= -32768:
        return b"\xd1" + struct.pack(">h", value)
    if value >= -(2 ** 31):
        return b"\xd2" + struct.pack(">i", value)
    return b"\xd3" + struct.pack(">q", value)


def mp_float(value: float) -> bytes:
    return b"\xcb" + struct.pack(">d", value)


def mp_str(text: str) -> bytes:
    raw = text.encode("utf-8")
    n = len(raw)
    if n < 32:
        head = bytes([0xA0 | n])
    elif n <= 0xFF:
        head = b"\xd9" + bytes([n])
    elif n <= 0xFFFF:
        head = b"\xda" + struct.pack(">H", n)
    else:
        head = b"\xdb" + struct.pack(">I", n)
    return head + raw


def mp_bool(value: bool) -> bytes:
    return b"\xc3" if value else b"\xc2"


def mp_map(pairs) -> bytes:
    n = len(pairs)
    if n < 16:
        head = bytes([0x80 | n])
    elif n <= 0xFFFF:
        head = b"\xde" + struct.pack(">H", n)
    else:
        head = b"\xdf" + struct.pack(">I", n)
    return head + b"".join(mp_str(k) + v for k, v in pairs)


def mp_array(items) -> bytes:
    n = len(items)
    if n < 16:
        head = bytes([0x90 | n])
    elif n <= 0xFFFF:
        head = b"\xdc" + struct.pack(">H", n)
    else:
        head = b"\xdd" + struct.pack(">I", n)
    return head + b"".join(items)


# ---------------------------------------------------------------------------
# v2 encoder
# ---------------------------------------------------------------------------

def encode_v2(inputs, *, version=2, input_tag="", author="fixture",
              description="AutoFPCount test fixture", duration=1.25,
              game_version=22081, framerate=240.0, seed=12345, coins=0,
              ldm=False, platformer=False, bot_name="FixtureBot",
              bot_version=1, level_id=0, level_name="Test Level",
              deaths=(), extension=b"") -> bytes:
    out = bytearray()
    out += b"GDR"
    out += varint(version)
    out += string(input_tag)
    out += string(author)
    out += string(description)
    out += be32(duration)
    out += varint(game_version)
    out += be64(framerate)
    out += varint(seed)
    out += varint(coins)
    out += varint(1 if ldm else 0)
    out += varint(1 if platformer else 0)
    out += string(bot_name)
    out += varint(bot_version)
    out += varint(level_id)
    out += string(level_name)

    out += varint(len(extension))
    out += extension

    out += varint(len(deaths))
    previous = 0
    for death in deaths:
        out += varint(death - previous)
        previous = death

    out += varint(len(inputs))
    p1 = sum(1 for i in inputs if not i[3])
    out += varint(p1)

    # A non-empty inputTag means every input is followed by its own length
    # prefixed extension blob, exactly as the writer emits it.
    per_input_ext = b"" if not input_tag else b"\x00"

    # Player 1 records first, then player 2, each with its own delta origin.
    for want_p2 in (False, True):
        previous = 0
        for frame, button, down, player2 in inputs:
            if player2 != want_p2:
                continue
            delta = frame - previous
            if platformer:
                packed = (delta << 3) | ((button & 3) << 1) | (1 if down else 0)
            else:
                packed = (delta << 1) | (1 if down else 0)
            out += varint(packed)
            out += per_input_ext
            previous = frame

    return bytes(out)


# ---------------------------------------------------------------------------
# v1 encoder
# ---------------------------------------------------------------------------

def encode_v1(inputs, *, version=1, author="fixture",
              description="AutoFPCount test fixture", duration=1.25,
              game_version=22081, framerate=240.0, seed=12345, coins=0,
              ldm=False, bot_name="FixtureBot", bot_version="1.0",
              level_id=0, level_name="Test Level", include_framerate=True):
    items = []
    for frame, button, down, player2 in inputs:
        items.append(mp_map([
            ("frame", mp_uint(frame)),
            ("btn", mp_uint(button)),
            ("2p", mp_bool(player2)),
            ("down", mp_bool(down)),
        ]))

    pairs = [
        ("author", mp_str(author)),
        ("bot", mp_map([("name", mp_str(bot_name)), ("version", mp_str(bot_version))])),
        ("coins", mp_uint(coins)),
        ("description", mp_str(description)),
        ("duration", mp_float(duration)),
    ]
    if include_framerate:
        pairs.append(("framerate", mp_float(framerate)))
    pairs += [
        ("gameVersion", mp_uint(game_version)),
        ("inputs", mp_array(items)),
        ("ldm", mp_bool(ldm)),
        ("level", mp_map([("id", mp_uint(level_id)), ("name", mp_str(level_name))])),
        ("seed", mp_uint(seed)),
        ("version", mp_float(version)),
    ]
    return mp_map(pairs)


# ---------------------------------------------------------------------------
# Fixtures. Each entry is (filename, bytes, expected-decoder-contract) where the
# contract is (ticks, declaredFps, note) and is asserted by CI.
# ---------------------------------------------------------------------------

def fixtures():
    out = []

    # A minimal classic-mode replay: three jump presses on player 1.
    simple = [(0, 1, True, False), (12, 1, True, False), (30, 1, True, False)]
    out.append(("simple.gdr2", encode_v2(simple), ([0, 12, 30], 240.0)))

    # Release events matter as much as presses: an input with down=false is a
    # "let go" and the analyser must see it.
    with_release = [
        (0, 1, True, False),
        (6, 1, False, False),
        (12, 1, True, False),
        (18, 1, False, False),
    ]
    out.append(("release.gdr2", encode_v2(with_release), ([0, 6, 12, 18], 240.0)))

    # Platformer mode switches to the 2-bit button packing. Same tick values,
    # different bytes - this is the case a parser that ignores the flag gets
    # wrong, and it fails loudly.
    platformer = [(0, 1, True, False), (5, 2, True, False), (9, 3, True, False)]
    out.append(("platformer.gdr2", encode_v2(platformer, platformer=True),
                ([0, 5, 9], 240.0)))

    # Large frame deltas force multi-byte varints, which is where a reader that
    # assumes one byte per chunk breaks.
    big_delta = [(0, 1, True, False), (5000, 1, True, False), (100000, 1, True, False)]
    out.append(("bigdelta.gdr2", encode_v2(big_delta), ([0, 5000, 100000], 240.0)))

    # Two players. Player 2's frames are delta encoded from its own origin, so
    # the decoder must reset the running frame at the p1/p2 boundary.
    two_player = [
        (0, 1, True, False),
        (10, 1, True, False),
        (4, 1, True, True),
        (9, 1, True, True),
    ]
    out.append(("twoplayer.gdr2", encode_v2(two_player), ([0, 4, 9, 10], 240.0)))

    # A non-empty inputTag means every input is followed by its own length
    # prefixed extension blob. A decoder that skips those bytes lands mid-field
    # and produces garbage.
    tagged = [(0, 1, True, False), (8, 1, True, False)]
    out.append(("inputext.gdr2",
                encode_v2(tagged, input_tag="FrameCounter", extension=b"\x01\x02"),
                ([0, 8], 240.0)))

    # Deaths are delta encoded; they do not affect inputs but must not derail
    # the input stream that follows.
    with_deaths = [(0, 1, True, False), (60, 1, True, False), (120, 1, True, False)]
    out.append(("deaths.gdr2", encode_v2(with_deaths, deaths=[15, 45, 100]),
                ([0, 60, 120], 240.0)))

    # Declared tick rate that is NOT 240. The importer must still decode it and
    # let the 240 gate reject it with FpsMismatch, rather than refusing to parse.
    out.append(("fps60.gdr2", encode_v2(simple, framerate=60.0), ([0, 12, 30], 60.0)))

    # Zero inputs: a valid, empty attempt.
    out.append(("empty.gdr2", encode_v2([], duration=0.0), ([], 240.0)))

    # v1 MessagePack, the canonical .gdr encoding.
    out.append(("simple.gdr", encode_v1(simple), ([0, 12, 30], 240.0)))

    # v1 without `framerate`. The format's stated default is 240, so the decoder
    # must fall back to 240 rather than reporting the rate as missing.
    out.append(("noframerate.gdr", encode_v1(simple, include_framerate=False),
                ([0, 12, 30], 240.0)))

    # v1 two-player, where the player-2 key is literally "2p".
    out.append(("twoplayer.gdr", encode_v1(two_player), ([0, 4, 9, 10], 240.0)))

    # v1 at 60 TPS: again, decode then reject at the gate, not a parse failure.
    out.append(("fps60.gdr", encode_v1(simple, framerate=60.0), ([0, 12, 30], 60.0)))

    # Deliberately corrupt, for the rejection paths.
    out.append(("truncated.gdr2", encode_v2(simple)[:-3], None))
    out.append(("badmagic.gdr2", b"XXX" + encode_v2(simple)[3:], None))
    out.append(("notgdr.bin", b"this is not a replay at all", None))

    return out


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2

    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)

    for name, blob, contract in fixtures():
        path = os.path.join(out_dir, name)
        with open(path, "wb") as handle:
            handle.write(blob)
        note = "expects %s" % (contract,) if contract else "must be rejected"
        print("  %-18s %6d bytes  %s" % (name, len(blob), note))

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
