# Block compression (BC1 / BC3 / BC4 / BC5) and DDS writing in numpy, for converting PNG scenes to compressed DDS.
# Simple and fast rather than best quality: endpoints along each block's main color direction (BC1), min / max
# (BC4), nearest palette entry per texel. Good enough for 4K photo textures; errors are reported by self_test.
import struct
import numpy as np

def to_blocks(image):
    """(h, w, c) -> (h/4, w/4, 16, c), padding to a multiple of 4 by repeating the last row / column."""
    h, w, c = image.shape
    ph, pw = (-h) % 4, (-w) % 4
    if ph or pw:
        image = np.pad(image, ((0, ph), (0, pw), (0, 0)), mode='edge')
    h, w = image.shape[:2]
    return image.reshape(h // 4, 4, w // 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(h // 4, w // 4, 16, c)

def _expand565(c):
    r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)

def encode_bc1(blocks):
    """blocks (..., 16, 3) float 0-255 -> (..., 8) uint8, always the 4-color mode (also valid as BC3's color part)."""
    x = blocks.astype(np.float32)
    mean = x.mean(-2, keepdims=True)
    d = x - mean
    cov = np.einsum('...ti,...tj->...ij', d, d)
    axis = np.ones(x.shape[:-2] + (3,), np.float32)
    for _ in range(6):  # power iteration: the main direction of the colors
        axis = np.einsum('...ij,...j->...i', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=-1, keepdims=True), 1e-12)
    t = np.einsum('...ti,...i->...t', d, axis)
    lo = mean[..., 0, :] + axis * t.min(-1, keepdims=True)
    hi = mean[..., 0, :] + axis * t.max(-1, keepdims=True)
    def quantize(c):
        c = np.clip(c, 0, 255)
        return ((np.round(c[..., 0] * 31 / 255).astype(np.uint32) << 11) |
                (np.round(c[..., 1] * 63 / 255).astype(np.uint32) << 5) |
                np.round(c[..., 2] * 31 / 255).astype(np.uint32))
    c0, c1 = quantize(hi), quantize(lo)
    swap = c0 < c1
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    p0, p1 = _expand565(c0), _expand565(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], -2)  # (..., 4, 3)
    dist = ((x[..., :, None, :] - palette[..., None, :, :]) ** 2).sum(-1)    # (..., 16, 4)
    idx = dist.argmin(-1).astype(np.uint32)
    idx = np.where((c0 == c1)[..., None], 0, idx)  # one color: index 0 everywhere
    bits = np.zeros(idx.shape[:-1], np.uint32)
    for t in range(16):
        bits |= idx[..., t] << (2 * t)
    out = np.zeros(idx.shape[:-1] + (8,), np.uint8)
    out[..., 0], out[..., 1] = c0 & 255, c0 >> 8
    out[..., 2], out[..., 3] = c1 & 255, c1 >> 8
    for i in range(4):
        out[..., 4 + i] = (bits >> (8 * i)) & 255
    return out

def encode_bc4(blocks):
    """blocks (..., 16) float 0-255 -> (..., 8) uint8: 8 values between max (e0) and min (e1)."""
    x = np.clip(blocks.astype(np.float32), 0, 255)
    e0 = np.round(x.max(-1)).astype(np.int32)
    e1 = np.round(x.min(-1)).astype(np.int32)
    palette = np.stack([e0, e1] + [((7 - k) * e0 + k * e1) / 7 for k in range(1, 7)], -1).astype(np.float32)
    idx = np.abs(x[..., :, None] - palette[..., None, :]).argmin(-1).astype(np.uint64)
    idx = np.where((e0 == e1)[..., None], 0, idx).astype(np.uint64)
    bits = np.zeros(idx.shape[:-1], np.uint64)
    for t in range(16):
        bits |= idx[..., t] << np.uint64(3 * t)
    out = np.zeros(idx.shape[:-1] + (8,), np.uint8)
    out[..., 0], out[..., 1] = e0, e1
    for i in range(6):
        out[..., 2 + i] = ((bits >> np.uint64(8 * i)) & np.uint64(255)).astype(np.uint8)
    return out

def encode(image, fmt, rows_per_chunk=64):
    """image (h, w, c) float 0-255 -> block bytes. fmt: 'DXT1' (RGB), 'DXT5' (RGBA), 'ATI2' (RG)."""
    blocks = to_blocks(image)
    parts = []
    for r in range(0, blocks.shape[0], rows_per_chunk):
        b = blocks[r:r + rows_per_chunk]
        if fmt == 'DXT1':
            parts.append(encode_bc1(b[..., :3]))
        elif fmt == 'DXT5':
            parts.append(np.concatenate([encode_bc4(b[..., 3]), encode_bc1(b[..., :3])], -1))
        elif fmt == 'ATI2':
            parts.append(np.concatenate([encode_bc4(b[..., 0]), encode_bc4(b[..., 1])], -1))
        else:
            raise ValueError(fmt)
    return np.concatenate(parts, 0).tobytes()

def srgb_to_linear(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)

def linear_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055) * 255.0

def mip_chain(image, srgb):
    """Full chain down to 1x1: 2x2 box filter (odd sizes drop the last row / column), colors averaged as light."""
    levels = [image]
    work = srgb_to_linear(image[..., :3]) if srgb else image[..., :3]
    alpha = image[..., 3:]
    while max(work.shape[:2]) > 1:
        h, w = max(work.shape[0] // 2, 1), max(work.shape[1] // 2, 1)
        def half(a):
            a = a[:h * 2 if a.shape[0] > 1 else 1, :w * 2 if a.shape[1] > 1 else 1]
            if a.shape[0] > 1:
                a = (a[0::2] + a[1::2]) * 0.5
            if a.shape[1] > 1:
                a = (a[:, 0::2] + a[:, 1::2]) * 0.5
            return a
        work, alpha = half(work), half(alpha)
        rgb = linear_to_srgb(work) if srgb else work
        levels.append(np.concatenate([rgb, alpha], -1))
    return levels

def write_dds(path, width, height, fmt, level_bytes):
    header = bytearray(128)
    header[0:4] = b'DDS '
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000 | (0x20000 if len(level_bytes) > 1 else 0)
    struct.pack_into('<IIIIIII', header, 4, 124, flags, height, width, len(level_bytes[0]), 0, len(level_bytes))
    struct.pack_into('<II4s', header, 76, 32, 0x4, fmt.encode())
    struct.pack_into('<I', header, 108, 0x1000 | (0x8 | 0x400000 if len(level_bytes) > 1 else 0))
    with open(path, 'wb') as f:
        f.write(header)
        for b in level_bytes:
            f.write(b)

# ---- decoding, only to measure the error
def decode_bc1(data, w, h):
    b = np.frombuffer(data, np.uint8).reshape((h + 3) // 4, (w + 3) // 4, 8)
    c0 = b[..., 0].astype(np.uint32) | (b[..., 1].astype(np.uint32) << 8)
    c1 = b[..., 2].astype(np.uint32) | (b[..., 3].astype(np.uint32) << 8)
    p0, p1 = _expand565(c0), _expand565(c1)
    palette = np.stack([p0, p1, (2 * p0 + p1) / 3, (p0 + 2 * p1) / 3], -2)
    bits = b[..., 4:8].copy().view(np.uint32)[..., 0]
    idx = np.stack([(bits >> (2 * t)) & 3 for t in range(16)], -1).astype(np.int64)
    texels = np.take_along_axis(palette, idx[..., None].repeat(3, -1), -2)
    bh, bw = b.shape[:2]
    return texels.reshape(bh, bw, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 3)[:h, :w]

def self_test():
    rng = np.random.default_rng(1)
    y, x = np.mgrid[0:64, 0:64].astype(np.float32)
    img = np.stack([x * 4, y * 4, (x + y) * 2], -1) + rng.normal(0, 4, (64, 64, 3))
    img = np.clip(img, 0, 255)
    dec = decode_bc1(encode(np.concatenate([img, np.full((64, 64, 1), 255.0)], -1), 'DXT1'), 64, 64)
    rmse = float(np.sqrt(((dec - img) ** 2).mean()))
    assert rmse < 8, rmse
    return rmse
