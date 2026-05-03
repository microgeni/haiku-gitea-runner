#!/usr/bin/env python3
"""
gen_icon.py — Generate the act_runner HVIF icon and write app_icon.rdef.

Run: python3 haiku-packaging/gen_icon.py

HVIF format — authoritative source:
    haiku/src/libs/icon/flat_icon/FlatIconFormat.cpp   (read_coord/write_coord)
    haiku/src/libs/icon/flat_icon/FlatIconFormat.h     (constants)
    haiku/src/libs/icon/flat_icon/FlatIconImporter.cpp (parser)
    haiku/src/libs/icon/flat_icon/PathCommandQueue.cpp (command packing)

Coordinate encoding (from write_coord):
    Simple 1-byte form (used when coord is integer AND -32 <= coord <= 95):
        byte = uint8(coord + 32.0)
        High bit CLEAR (bit 7 = 0)
    2-byte form (for non-integer or out-of-range):
        uint16 = uint16((coord + 128.0) * 102.0) | 32768
        written as two bytes, high byte first with bit 15 set

    We use ONLY integer coords in [-28..+28] → always 1-byte.

Path command packing (from PathCommandQueue):
    2 bits per command, 4 commands per byte, LSB first.
    commandBufferSize = (pointCount + 3) / 4
    Commands:
        PATH_COMMAND_H_LINE = 0  (1 coord: x only)
        PATH_COMMAND_V_LINE = 1  (1 coord: y only)
        PATH_COMMAND_LINE   = 2  (2 coords: x, y)
        PATH_COMMAND_CURVE  = 3  (6 coords: point xy + pointIn xy + pointOut xy)
    NO MOVE_TO — first point is implicit start.

Path flags (FlatIconFormat.h):
    PATH_FLAG_CLOSED        = 1<<1 = 0x02
    PATH_FLAG_USES_COMMANDS = 1<<2 = 0x04
    PATH_FLAG_NO_CURVES     = 1<<3 = 0x08

Shape record — _ReadPathSourceShape() field order:
    uint8  styleIndex       ← style index FIRST
    uint8  pathCount
    uint8  pathIndex[]      (pathCount entries)
    uint8  shapeFlags       (0x00 = identity)
"""

import os, subprocess

# ── Constants ─────────────────────────────────────────────────────────────────

FLAT_ICON_MAGIC        = b'ncif'   # 'ficn' reversed (little-endian uint32)
STYLE_TYPE_SOLID_COLOR = 1
SHAPE_TYPE_PATH_SOURCE = 10
PATH_FLAG_CLOSED        = 0x02
PATH_FLAG_USES_COMMANDS = 0x04
PATH_FLAG_NO_CURVES     = 0x08
PATH_COMMAND_H_LINE     = 0
PATH_COMMAND_V_LINE     = 1
PATH_COMMAND_LINE       = 2
PATH_COMMAND_CURVE      = 3

# ── Coordinate encoder ────────────────────────────────────────────────────────

def enc(v: float) -> int:
    """
    Encode coord using simple 1-byte form: byte = int(v + 32).
    Valid for integer values in [-32, 95]. High bit stays clear.
    """
    b = int(round(v + 32.0))
    if b < 0 or b > 127:
        raise ValueError(f"coord {v} → byte {b} out of 1-byte range [0,127]")
    return b

def dec(b: int) -> float:
    """Decode 1-byte coord."""
    return float(b) - 32.0

# ── Command packer ────────────────────────────────────────────────────────────

def pack_commands(commands: list) -> bytes:
    """2 bits per command, 4 per byte, LSB-first. Size = ceil(n/4) bytes."""
    buf_size = (len(commands) + 3) // 4
    buf = bytearray(buf_size)
    for i, cmd in enumerate(commands):
        buf[i // 4] |= (cmd & 0x03) << ((i % 4) * 2)
    return bytes(buf)

# ── Style ─────────────────────────────────────────────────────────────────────

def solid(r: int, g: int, b: int, a: int = 255) -> bytes:
    return bytes([STYLE_TYPE_SOLID_COLOR, r, g, b, a])

# ── Path ──────────────────────────────────────────────────────────────────────

def make_path_full_curves(points_with_handles: list, closed: bool = True) -> bytes:
    """
    Encode a path in FULL CURVES mode (no flags, no command buffer).
    Each entry: (px, py, in_x, in_y, out_x, out_y)
    This is the mode Icon-O-Matic uses for smooth shapes.
    flags = PATH_FLAG_CLOSED (0x02) only, or 0 if open.
    """
    flags = PATH_FLAG_CLOSED if closed else 0
    n = len(points_with_handles)
    buf = bytearray([flags, n])
    for (px, py, ix, iy, ox, oy) in points_with_handles:
        buf += bytes([enc(px), enc(py), enc(ix), enc(iy), enc(ox), enc(oy)])
    return bytes(buf)

def make_path_no_curves(points: list, closed: bool = True) -> bytes:
    """
    Encode a path with NO curves (straight lines only).
    flags = PATH_FLAG_CLOSED | PATH_FLAG_NO_CURVES (0x0A)
    Each entry: (x, y)
    """
    flags = PATH_FLAG_NO_CURVES | (PATH_FLAG_CLOSED if closed else 0)
    n = len(points)
    buf = bytearray([flags, n])
    for (x, y) in points:
        buf += bytes([enc(x), enc(y)])
    return bytes(buf)
    """
    Build a PATH_FLAG_USES_COMMANDS path.

    point_cmds: list of tuples:
        (PATH_COMMAND_LINE,   x, y)
        (PATH_COMMAND_H_LINE, x)
        (PATH_COMMAND_V_LINE, y)
        (PATH_COMMAND_CURVE,  px, py, in_x, in_y, out_x, out_y)
    """
    flags = PATH_FLAG_USES_COMMANDS | (PATH_FLAG_CLOSED if closed else 0)
    cmds  = [pc[0] for pc in point_cmds]
    n     = len(cmds)

    coord_buf = bytearray()
    for entry in point_cmds:
        cmd = entry[0]
        v   = entry[1:]
        if cmd == PATH_COMMAND_H_LINE:
            coord_buf.append(enc(v[0]))
        elif cmd == PATH_COMMAND_V_LINE:
            coord_buf.append(enc(v[0]))
        elif cmd == PATH_COMMAND_LINE:
            coord_buf.append(enc(v[0]))
            coord_buf.append(enc(v[1]))
        elif cmd == PATH_COMMAND_CURVE:
            for coord in v:          # px,py, in_x,in_y, out_x,out_y
                coord_buf.append(enc(coord))

    buf = bytearray([flags, n])
    buf += pack_commands(cmds)
    buf += coord_buf
    return bytes(buf)

# ── Shape ─────────────────────────────────────────────────────────────────────

def make_shape(style_idx: int, path_indices: list, shape_flags: int = 0) -> bytes:
    """Field order: styleIndex, pathCount, pathIndex[], shapeFlags."""
    buf = bytearray([SHAPE_TYPE_PATH_SOURCE, style_idx, len(path_indices)])
    buf.extend(path_indices)
    buf.append(shape_flags)
    return bytes(buf)

# ── Assembler ─────────────────────────────────────────────────────────────────

def make_hvif(styles, paths, shapes) -> bytes:
    buf = bytearray(FLAT_ICON_MAGIC)
    buf.append(len(styles));  [buf.extend(s) for s in styles]
    buf.append(len(paths));   [buf.extend(p) for p in paths]
    buf.append(len(shapes));  [buf.extend(s) for s in shapes]
    return bytes(buf)

# ── Geometry ──────────────────────────────────────────────────────────────────
#
# All coordinates are INTEGERS (required for 1-byte encoding).
# Canvas: each unit ≈ 1px on a 64×64 icon. Origin at top-left in screen
# space, but HVIF coords run -32..+95 with 0 near top-left.
# We centre our design around coord (0,0) = HVIF byte 32.
#
# Rounded rectangle:
#   Outer bounds: ±28  (56 units wide/tall, leaving ~4 units margin on 64px canvas)
#   Corner radius: 8
#   Bézier kappa offset: 4 (integer approx of 8 × 0.5523 ≈ 4.4)
#
# 8-point path: 4 × (LINE edge-endpoint + CURVE corner-endpoint)
# Starting at the top-right end of the top edge, going clockwise.
#
# Each CURVE corner point:
#   point  = the on-curve corner tangent point
#   pointIn  = incoming handle (from the previous straight edge)
#   pointOut = outgoing handle (toward the next straight edge)
#
# Corner top-right (arriving from left along top, departing downward along right):
#   on-curve point: (+28, -20)
#   in-handle: (+28, -24)   [pointing up, along right edge]
#   out-handle: (+28, -16)  [pointing down, along right edge]
#
# Wait -- the handles are for the BEZIER CURVE leading INTO and OUT OF this point.
# The segment from the top edge LINE point (+20,-28) to this CURVE point (+28,-20):
#   That segment's control points are:
#     cp1 = (+20+4, -28) = (+24, -28)   [attached to the LINE point's out-handle]
#     cp2 = (+28, -20-4) = (+28, -24)   [= this CURVE point's in-handle]
#
# So: pointIn = (+28, -24), which is the cp2 of the arc from (+20,-28)→(+28,-20).
# And: pointOut = mirrored = (+28, -20+4) = (+28, -16), which is cp1 for the
#   arc from (+28,-20)→(+28,+20) -- but (+28,+20) is a LINE point, so
#   the LINE segment uses pointOut of (+28,-20) as its cp1... but LINE
#   points have pointIn=pointOut=point (no handles). So pointOut doesn't matter
#   for a segment ending at a LINE point. We set it to mirror pointIn anyway.

W = 28    # half-extent
R =  8    # corner radius
k =  4    # kappa offset (integer approx of 8 × 0.5523 ≈ 4.4)

# Rounded rectangle — full-curves mode (6 bytes per point: xy + in + out).
# 8 points alternating edge-midpoints and corner-arc-endpoints.
# Edge midpoints: sharp (in/out point toward adjacent corners).
# Corner endpoints: smooth (in from previous edge, out toward next edge).
rr_path = make_path_full_curves([
    # top edge right-end (+20,-28): in from left, out toward top-right corner
    (+20, -28,  +16, -28,  +24, -28),
    # top-right corner (+28,-20): in from top edge, out down right edge
    (+28, -20,  +28, -24,  +28, -16),
    # right edge bottom-end (+28,+20): in from above, out toward corner
    (+28, +20,  +28, +16,  +28, +24),
    # bottom-right corner (+20,+28): in from right edge, out left along bottom
    (+20, +28,  +24, +28,  +16, +28),
    # bottom edge left-end (-20,+28): in from right, out toward bottom-left corner
    (-20, +28,  -16, +28,  -24, +28),
    # bottom-left corner (-28,+20): in from bottom edge, out up left edge
    (-28, +20,  -28, +24,  -28, +16),
    # left edge top-end (-28,-20): in from below, out toward top-left corner
    (-28, -20,  -28, -16,  -28, -24),
    # top-left corner (-20,-28): in from left edge, out right along top
    (-20, -28,  -24, -28,  -16, -28),
], closed=True)

# Triangle — no-curves mode (straight lines, 2 bytes per point)
tri_path = make_path_no_curves([
    (-13, -18),
    (+18,   0),
    (-13, +18),
], closed=True)

# ── Assemble ──────────────────────────────────────────────────────────────────

styles = [
    solid(0xFF, 0x7B, 0x29),   # 0: Gitea orange #FF7B29
    solid(0xFF, 0xFF, 0xFF),   # 1: white
]
paths  = [rr_path, tri_path]
shapes = [
    make_shape(style_idx=0, path_indices=[0]),
    make_shape(style_idx=1, path_indices=[1]),
]

icon_data = make_hvif(styles, paths, shapes)
assert icon_data[:4] == FLAT_ICON_MAGIC

# ── Verify: decode and print ──────────────────────────────────────────────────

def verify(data: bytes):
    i = 4
    sc = data[i]; i += 1
    print(f"  styles: {sc}")
    for s in range(sc):
        t = data[i]; i+=1
        r,g,b,a = data[i],data[i+1],data[i+2],data[i+3]; i+=4
        print(f"    [{s}] SOLID #{r:02X}{g:02X}{b:02X} a={a}")
    pc = data[i]; i += 1
    print(f"  paths: {pc}")
    CNAMES = {0:'H',1:'V',2:'L',3:'C'}
    for p in range(pc):
        flags = data[i]; i+=1
        npts  = data[i]; i+=1
        has_cmd = bool(flags & PATH_FLAG_USES_COMMANDS)
        closed  = bool(flags & PATH_FLAG_CLOSED)
        print(f"    [{p}] flags=0x{flags:02X} npts={npts} closed={closed} cmds={has_cmd}")
        if has_cmd:
            cb = (npts+3)//4
            raw = data[i:i+cb]; i += cb
            cmds = []
            for byte in raw:
                for bit in range(0,8,2): cmds.append((byte>>bit)&3)
            cmds = cmds[:npts]
            print(f"       cmd_bytes={raw.hex()} cmds={[CNAMES[c] for c in cmds]}")
            for cmd in cmds:
                if cmd == 0:   # H_LINE
                    x=dec(data[i]); i+=1
                    print(f"       H_LINE x={x}")
                elif cmd == 1:  # V_LINE
                    y=dec(data[i]); i+=1
                    print(f"       V_LINE y={y}")
                elif cmd == 2:  # LINE
                    x=dec(data[i]); y=dec(data[i+1]); i+=2
                    print(f"       LINE   ({x},{y})")
                elif cmd == 3:  # CURVE
                    px=dec(data[i]);  py=dec(data[i+1])
                    ix=dec(data[i+2]);iy=dec(data[i+3])
                    ox=dec(data[i+4]);oy=dec(data[i+5]); i+=6
                    print(f"       CURVE  pt=({px},{py}) in=({ix},{iy}) out=({ox},{oy})")
    shc = data[i]; i+=1
    print(f"  shapes: {shc}")
    for s in range(shc):
        stype=data[i]; i+=1
        si=data[i]; i+=1
        np2=data[i]; i+=1
        pis=list(data[i:i+np2]); i+=np2
        sf=data[i]; i+=1
        print(f"    [{s}] PATH_SOURCE style={si} paths={pis} flags=0x{sf:02X}")
    ok = i == len(data)
    print(f"  offset {i}/{len(data)} {'✓' if ok else '⚠ MISMATCH'}")

# ── Output ────────────────────────────────────────────────────────────────────

def to_rdef(data: bytes) -> str:
    hx = data.hex().upper()
    lines = ['resource vector_icon {']
    for i in range(0, len(hx), 32):
        lines.append(f'    $"{hx[i:i+32]}"')
    lines.append('};')
    return '\n'.join(lines)

HEADER = """\
// app_icon.rdef -- act_runner vector icon for Haiku OS
// Generated by gen_icon.py -- DO NOT EDIT MANUALLY
// Compile:  rc -o app_icon.rsrc haiku-packaging/app_icon.rdef
// Embed:    xres -o build/act_runner app_icon.rsrc

"""

if __name__ == '__main__':
    here = os.path.dirname(os.path.abspath(__file__))
    rdef = os.path.join(here, 'app_icon.rdef')
    hvif = os.path.join(here, 'act_runner.hvif')

    with open(rdef, 'w') as f:
        f.write(HEADER + to_rdef(icon_data) + '\n')
    with open(hvif, 'wb') as f:
        f.write(icon_data)
    subprocess.run(['addattr','-t','mime','BEOS:TYPE','image/x-hvif', hvif],
                   capture_output=True)

    print(f"HVIF: {len(icon_data)} bytes")
    print()
    print(to_rdef(icon_data))
    print()
    verify(icon_data)
    print()
    print(f"Written: {rdef}")
    print(f"Written: {hvif}")
