# DDS helpers for the Bistro conversion (numpy; runs inside Blender's Python).
import struct
import numpy as np

def read_header(path):
    with open(path, 'rb') as f:
        head = f.read(148)
    assert head[:4] == b'DDS ', path
    height, width = struct.unpack('<II', head[12:20])
    mips = max(1, struct.unpack('<I', head[28:32])[0])
    four = head[84:88]
    offset = 128
    if four == b'DX10':
        dxgi = struct.unpack('<I', head[128:132])[0]
        offset = 148
        four = {71: b'DXT1', 72: b'DXT1', 77: b'DXT5', 78: b'DXT5', 83: b'ATI2', 84: b'ATI2'}.get(dxgi, b'?%d' % dxgi)
    if four in (b'BC5U', b'BC5S'):
        four = b'ATI2'
    return dict(width=width, height=height, mips=mips, format=four.decode('latin1'), offset=offset)

def block_size(fmt):
    return 8 if fmt == 'DXT1' else 16

def level0_blocks(path, info):
    bw, bh = (info['width'] + 3) // 4, (info['height'] + 3) // 4
    n = bw * bh * block_size(info['format'])
    with open(path, 'rb') as f:
        f.seek(info['offset'])
        data = f.read(n)
    return np.frombuffer(data, np.uint8).reshape(bh, bw, block_size(info['format']))

def decode_channel_block(blocks8):
    """BC4-style 8-byte channel blocks (..., 8) -> values (..., 16) uint8 (texel order row-major in the block)."""
    e0 = blocks8[..., 0].astype(np.int32)
    e1 = blocks8[..., 1].astype(np.int32)
    bits = np.zeros(blocks8.shape[:-1], np.uint64)
    for i in range(6):
        bits |= blocks8[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    idx = np.stack([((bits >> np.uint64(3 * t)) & np.uint64(7)).astype(np.int32) for t in range(16)], -1)
    palette = np.zeros(blocks8.shape[:-1] + (8,), np.int32)
    palette[..., 0] = e0
    palette[..., 1] = e1
    eight = (e0 > e1)
    for k in range(2, 8):
        p8 = ((8 - k) * e0 + (k - 1) * e1) // 7
        p6 = ((6 - k) * e0 + (k - 1) * e1) // 5 if k < 6 else (0 if k == 6 else 255)
        palette[..., k] = np.where(eight, p8, p6)
    return np.take_along_axis(palette, idx, -1).astype(np.uint8)

def bc3_alpha(path):
    """Level-0 alpha of a DXT5 texture as a (blocks, 16) array; None for DXT1."""
    info = read_header(path)
    if info['format'] != 'DXT5':
        return None
    blocks = level0_blocks(path, info)
    return decode_channel_block(blocks[..., :8]).reshape(-1, 16)

def flip_channel_blocks(blocks8):
    """Exact v -> 255 - v on BC4-style channel blocks: invert and swap the endpoints (which keeps the block's
    mode), then renumber the indices to point at the same (inverted) palette entries."""
    out = blocks8.copy()
    e0 = blocks8[..., 0].astype(np.int32)
    e1 = blocks8[..., 1].astype(np.int32)
    out[..., 0] = (255 - e1).astype(np.uint8)
    out[..., 1] = (255 - e0).astype(np.uint8)
    bits = np.zeros(blocks8.shape[:-1], np.uint64)
    for i in range(6):
        bits |= blocks8[..., 2 + i].astype(np.uint64) << np.uint64(8 * i)
    eight = (e0 > e1)
    map8 = np.array([1, 0, 7, 6, 5, 4, 3, 2], np.uint64)   # 8 interpolated values: k -> 9 - k
    map6 = np.array([1, 0, 5, 4, 3, 2, 7, 6], np.uint64)   # 6 values + 0 and 255: those two swap too
    newbits = np.zeros_like(bits)
    for t in range(16):
        idx = ((bits >> np.uint64(3 * t)) & np.uint64(7)).astype(np.int64)
        new = np.where(eight, map8[idx], map6[idx])
        newbits |= new.astype(np.uint64) << np.uint64(3 * t)
    for i in range(6):
        out[..., 2 + i] = ((newbits >> np.uint64(8 * i)) & np.uint64(255)).astype(np.uint8)
    return out

def decode_bc5(path):
    """Level 0 of a BC5 texture as two float images in [-1, 1] (X, Y)."""
    info = read_header(path)
    b = level0_blocks(path, info)
    bh, bw = b.shape[:2]
    out = []
    for k in (0, 8):
        v = decode_channel_block(b[..., k:k + 8]).reshape(bh, bw, 4, 4).transpose(0, 2, 1, 3).reshape(bh * 4, bw * 4)
        out.append(v.astype(np.float32) / 127.5 - 1)
    return out

def y_up_score(path):
    """Which way a normal map's Y points. It comes from a height h: X = -dh/dx, Y = -dh/dy with y up the image,
    so dX/dy = dY/dx (no curl); with Y pointing down the image (DirectX) the two have opposite signs.
    Returns their correlation: clearly > 0 = Y up (OpenGL / glTF), < 0 = Y down, near 0 = can't tell."""
    x, y = decode_bc5(path)
    dx_dyup = -(x[1:, :-1] - x[:-1, :-1])   # rows go down the image
    dy_dx = y[:-1, 1:] - y[:-1, :-1]
    return float((dx_dyup * dy_dx).mean() / (np.sqrt((dx_dyup ** 2).mean() * (dy_dx ** 2).mean()) + 1e-12))

def write_bc5(src, dst, flip_green):
    """Copy a BC5 normal map (level 0 only), optionally with its green channel inverted (DirectX -> OpenGL)."""
    info = read_header(src)
    assert info['format'] == 'ATI2', (src, info['format'])
    blocks = level0_blocks(src, info).copy()
    if flip_green:
        blocks[..., 8:16] = flip_channel_blocks(blocks[..., 8:16])
    header = bytearray(128)
    header[0:4] = b'DDS '
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000   # caps, height, width, pixelformat, linearsize
    struct.pack_into('<IIIIIII', header, 4, 124, flags, info['height'], info['width'], blocks.size, 0, 1)
    struct.pack_into('<II4s', header, 76, 32, 0x4, b'ATI2')   # pixel format: size, FOURCC
    struct.pack_into('<I', header, 108, 0x1000)               # caps: texture
    with open(dst, 'wb') as f:
        f.write(header)
        f.write(blocks.tobytes())

def self_test():
    rng = np.random.default_rng(1)
    b = rng.integers(0, 256, (1000, 8), dtype=np.uint8)
    b[:500, 0] = np.maximum(b[:500, 0], b[:500, 1] + 1).clip(0, 255)  # some of each mode
    v = decode_channel_block(b).astype(np.int32)
    w = decode_channel_block(flip_channel_blocks(b)).astype(np.int32)
    err = np.abs((255 - v) - w).max()
    assert err <= 1, err   # integer rounding of the interpolation
    return err
