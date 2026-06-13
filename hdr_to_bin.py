#!/usr/bin/env python3
"""
Convert Poly Haven HDR (32-bit RLE RGBE) → downsampled float32 binary.
Output: 128x64x3 float32 per file, raw binary (row-major, R-G-B interleaved per pixel).

Usage: python3 hdr_to_bin.py public/hdri/foo_1k.hdr public/hdri_128x64/foo.bin
"""

import struct, sys, math, os

def read_rgbe_rle(f):
    """Read a 32-bit_rle_rgbe HDR file, return (width, height, pixels_flat_rgb_f32)."""
    # Parse header
    header = []
    while True:
        line = f.readline()
        if not line:
            break
        line = line.decode('ascii', 'ignore').strip()
        header.append(line)
        if not line or line == '':
            break

    # Parse resolution line
    res_line = f.readline().decode('ascii', 'ignore').strip()
    parts = res_line.split()
    # Format: "-Y 512 +X 1024" (space-separated) or "-Y512 +X1024" (concatenated)
    height = width = 0
    for i, p in enumerate(parts):
        if p in ('-Y', '+Y'):
            height = int(parts[i+1])
        elif p in ('-X', '+X'):
            width = int(parts[i+1])
        elif len(p) > 2 and (p.startswith('-Y') or p.startswith('+Y')):
            height = int(p[2:])
        elif len(p) > 2 and (p.startswith('-X') or p.startswith('+X')):
            width = int(p[2:])

    if width == 0 or height == 0:
        raise ValueError(f"Could not parse resolution: {res_line}")

    pixels = []
    for y in range(height):
        # Read scanline
        peek = f.read(2)
        if len(peek) < 2:
            break
        r1, r2 = peek[0], peek[1]

        if r1 == 2 and r2 == 2:
            # New-style RLE: read scanline
            scan_width_bytes = f.read(2)
            scan_w = (scan_width_bytes[0] << 8) | scan_width_bytes[1]
            if scan_w != width:
                raise ValueError(f"Scanline width mismatch: {scan_w} != {width}")

            # Read 4 channels, each RLE compressed
            channels = [bytearray(width) for _ in range(4)]
            for ch in range(4):
                pos = 0
                while pos < width:
                    code = f.read(1)[0]
                    if code > 128:
                        # Run: repeat next byte (code - 128) times
                        count = code - 128
                        val = f.read(1)[0]
                        for _ in range(count):
                            if pos < width:
                                channels[ch][pos] = val
                                pos += 1
                    else:
                        # Literal: next 'code' bytes are literal
                        count = code
                        if count == 0:
                            break
                        lit = f.read(count)
                        for b in lit:
                            if pos < width:
                                channels[ch][pos] = b
                                pos += 1

            # Decode RGBE for this scanline
            for x in range(width):
                r, g, b, e = channels[0][x], channels[1][x], channels[2][x], channels[3][x]
                if e == 0:
                    pixels.extend([0.0, 0.0, 0.0])
                else:
                    scale = math.ldexp(1.0, e - 128 - 8)  # 2^(e-128) / 256
                    pixels.append(r * scale)
                    pixels.append(g * scale)
                    pixels.append(b * scale)
        else:
            # Old-style RLE or uncompressed — read rest of scanline
            scan = peek + f.read(width * 4 - 2)
            if len(scan) < width * 4:
                break
            # Try old-style RLE
            pos = 0
            while pos < len(scan) and (pos // 4) < width:
                if scan[pos] == 1 and scan[pos+1] == 1 and scan[pos+2] == 1:
                    # Run
                    count = scan[pos+3]
                    r, g, b = scan[pos], scan[pos+1], scan[pos+2]
                    pos += 4
                    for _ in range(count):
                        if len(pixels) // 3 < width * (y + 1):
                            e = scan[pos] if pos < len(scan) else 0
                            scale = math.ldexp(1.0, e - 128 - 8) if e > 0 else 0
                            pixels.append(r * scale)
                            pixels.append(g * scale)
                            pixels.append(b * scale)
                            pos += 1
                else:
                    r, g, b, e = scan[pos], scan[pos+1], scan[pos+2], scan[pos+3]
                    pos += 4
                    scale = math.ldexp(1.0, e - 128 - 8) if e > 0 else 0
                    pixels.append(r * scale)
                    pixels.append(g * scale)
                    pixels.append(b * scale)

    return width, height, pixels


def downsample_bilinear(src, src_w, src_h, dst_w, dst_h):
    """Bilinear downsampling. src is list of floats (rgb interleaved)."""
    dst = []
    for y in range(dst_h):
        for x in range(dst_w):
            # Map destination pixel center to source coordinates
            sx = (x + 0.5) * src_w / dst_w - 0.5
            sy = (y + 0.5) * src_h / dst_h - 0.5

            x0 = max(0, min(src_w - 1, int(math.floor(sx))))
            y0 = max(0, min(src_h - 1, int(math.floor(sy))))
            x1 = min(src_w - 1, x0 + 1)
            y1 = min(src_h - 1, y0 + 1)

            fx = sx - x0
            fy = sy - y0

            for ch in range(3):
                idx00 = (y0 * src_w + x0) * 3 + ch
                idx10 = (y0 * src_w + x1) * 3 + ch
                idx01 = (y1 * src_w + x0) * 3 + ch
                idx11 = (y1 * src_w + x1) * 3 + ch

                v = (src[idx00] * (1 - fx) + src[idx10] * fx) * (1 - fy) + \
                    (src[idx01] * (1 - fx) + src[idx11] * fx) * fy
                dst.append(v)
    return dst


def main():
    import glob
    DST_W, DST_H = 256, 128
    out_dir = f'public/hdri_{DST_W}x{DST_H}'
    os.makedirs(out_dir, exist_ok=True)

    hdr_files = sorted(glob.glob('public/hdri/*.hdr'))
    if not hdr_files:
        print("No .hdr files found")
        return

    for hdr_path in hdr_files:
        basename = os.path.splitext(os.path.basename(hdr_path))[0]
        out_path = f'{out_dir}/{basename}.bin'

        print(f"Processing {basename}...")

        with open(hdr_path, 'rb') as f:
            src_w, src_h, src_pixels = read_rgbe_rle(f)

        print(f"  Source: {src_w}x{src_h}, pixels: {len(src_pixels)//3}")

        # Downsample to target
        dst_pixels = downsample_bilinear(src_pixels, src_w, src_h, DST_W, DST_H)

        # Robust normalization using percentiles to prevent bleaching from outliers
        flat = sorted(dst_pixels)
        n = len(dst_pixels)
        p50  = flat[int(n * 0.50)]   # median
        p999 = flat[int(n * 0.999)]  # 99.9th percentile
        p9999 = flat[max(0, int(n * 0.9999))]  # 99.99th
        max_val = flat[-1]

        print(f"  median={p50:.4f}  99.9%={p999:.4f}  99.99%={p9999:.4f}  max={max_val:.4f}")

        # Scale so 99.9th percentile maps to ~4.0 (comfortable for ACES)
        # Then CLAMP at 3x the 99.9th percentile to kill outlier bleaching
        target = 4.0
        scale = target / max(p999, 0.001)
        clamp = max(p9999 * 1.2, target * 3.0) * scale  # generous cap

        print(f"  scale={scale:.4f}  clamp={clamp:.2f}")

        # Apply normalization with clamping
        normalized = [min(v * scale, clamp) for v in dst_pixels]

        # Verify post-normalization stats
        flat2 = sorted(normalized)
        print(f"  post: 99.9%={flat2[int(n*0.999)]:.2f}  max={flat2[-1]:.2f}  over10={sum(1 for v in normalized if v>10)}/{n}")

        # Write float32 binary
        f32 = struct.pack(f'{len(normalized)}f', *normalized)
        with open(out_path, 'wb') as outf:
            outf.write(f32)
        print(f"  → {out_path} ({len(f32)} bytes)")

    print(f"\nDone! {len(hdr_files)} files converted.")


if __name__ == '__main__':
    main()
