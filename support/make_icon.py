#!/usr/bin/env python3
"""
Generate DiskPart.info — a valid Amiga WBTool icon.

Produces a 32x40, 2-bitplane icon with a simple hard-drive graphic.
Run from the repo root:  python3 support/make_icon.py
Output: DiskPart.info
"""

import struct, os, sys

# --- Image dimensions -------------------------------------------------
W, H, DEPTH = 32, 40, 2
WPL = (W + 15) // 16   # words per line = 2

# Colour indices (2-bitplane encoding):
#   0 = 00 = screen background (Workbench grey)
#   1 = 01 = colour 1 (black)
#   2 = 10 = colour 2 (white / light)
#   3 = 11 = colour 3 (Workbench blue highlight)

def make_icon_grid():
    g = [[0] * W for _ in range(H)]

    def hline(y, x0, x1, c):
        for x in range(x0, x1):
            g[y][x] = c

    def vline(x, y0, y1, c):
        for y in range(y0, y1):
            g[y][x] = c

    def rect(y0, x0, y1, x1, c):
        hline(y0, x0, x1, c)
        hline(y1, x0, x1, c)
        vline(x0, y0, y1 + 1, c)
        vline(x1 - 1, y0, y1 + 1, c)

    # Outer case border
    rect(1, 0, H - 2, W, 1)

    # Fill body with light colour
    for y in range(2, H - 2):
        hline(y, 1, W - 1, 2)

    # Top cap: narrow strip across the top
    for y in range(2, 6):
        hline(y, 1, W - 1, 2)
    hline(6, 1, W - 1, 1)

    # Activity LED (top-right corner of cap)
    for y in range(3, 6):
        for x in range(W - 6, W - 3):
            g[y][x] = 3

    # Model label area (top-left of cap, lighter block)
    for y in range(3, 6):
        for x in range(2, 12):
            g[y][x] = 2

    # Platter tracks (horizontal stripes through body)
    for track_y in range(10, H - 4, 5):
        hline(track_y, 2, W - 2, 1)
        hline(track_y + 1, 2, W - 2, 3)

    # Read/write arm (diagonal-ish element)
    for i in range(8):
        x = 6 + i * 2
        y = 16 + i
        if 0 <= x < W and 0 <= y < H:
            g[y][x] = 1
        if 0 <= x + 1 < W:
            g[y][x + 1] = 1

    return g


def invert_grid(g):
    """Selected-state: swap colours 1 and 2, keep 0 and 3."""
    swap = {0: 0, 1: 2, 2: 1, 3: 3}
    return [[swap[c] for c in row] for row in g]


def grid_to_bitplanes(g):
    """Encode grid as planar bitplane data (plane 0 all rows, then plane 1)."""
    p0 = bytearray()
    p1 = bytearray()
    for row in g:
        for w in range(WPL):
            b0 = b1 = 0
            for bit in range(16):
                px = w * 16 + bit
                if px < W:
                    c = row[px]
                    if c & 1:
                        b0 |= 0x8000 >> bit
                    if c & 2:
                        b1 |= 0x8000 >> bit
            p0 += struct.pack('>H', b0)
            p1 += struct.pack('>H', b1)
    return bytes(p0) + bytes(p1)


def pack_image_struct(w, h, depth, has_data, has_next):
    """Return a packed Image struct (20 bytes)."""
    return struct.pack('>hhhhhIBBI',
        0,                      # LeftEdge
        0,                      # TopEdge
        w,                      # Width
        h,                      # Height
        depth,                  # Depth
        1 if has_data else 0,   # ImageData (non-NULL = data follows)
        (1 << depth) - 1,       # PlanePick (all planes)
        0,                      # PlaneOnOff
        1 if has_next else 0,   # NextImage (non-NULL = another Image follows)
    )


def pack_gadget_struct(w, h, has_default, has_select):
    """Return a packed Gadget struct (44 bytes)."""
    return struct.pack('>IhhHHHHHIIIIIHI',
        0,                              # NextGadget
        0, 0,                           # LeftEdge, TopEdge
        w, h,                           # Width, Height
        0x0006,                         # Flags: GADGIMAGE | GADGHIMAGE
        0x0000,                         # Activation
        0x0001,                         # GadgetType: GTYP_BOOLGADGET
        1 if has_default else 0,        # GadgetRender
        1 if has_select else 0,         # SelectRender
        0, 0, 0,                        # GadgetText, MutualExclude, SpecialInfo
        0, 0,                           # GadgetID, UserData
    )


# --- Assemble the .info file ------------------------------------------

default_grid = make_icon_grid()
select_grid  = invert_grid(default_grid)
default_data = grid_to_bitplanes(default_grid)
select_data  = grid_to_bitplanes(select_grid)

out = bytearray()

# DiskObject (78 bytes total)
out += struct.pack('>HH', 0xE310, 1)            # Magic, Version
out += pack_gadget_struct(W, H, True, True)      # Gadget (44 bytes)
out += bytes([3, 0])                             # do_Type=WBTOOL + 1 pad byte
out += struct.pack('>IIIIII',
    0,           # do_DefaultTool  (NULL)
    0,           # do_ToolTypes    (NULL)
    0x80000000,  # do_CurrentX     (NO_ICON_POSITION)
    0x80000000,  # do_CurrentY     (NO_ICON_POSITION)
    0,           # do_DrawerData   (NULL)
    0,           # do_ToolWindow   (NULL)
)
out += struct.pack('>I', 8192)                   # do_StackSize

assert len(out) == 78, f"DiskObject size wrong: {len(out)}"

# Default image
out += pack_image_struct(W, H, DEPTH, has_data=True, has_next=False)
out += default_data

# Selected image
out += pack_image_struct(W, H, DEPTH, has_data=True, has_next=False)
out += select_data

# Write
dest = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    'DiskPart.info')
with open(dest, 'wb') as f:
    f.write(out)

print(f"Written {dest} ({len(out)} bytes)")
