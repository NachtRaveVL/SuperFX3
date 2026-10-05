#!/usr/bin/env python3
"""Convert an FFmpeg-readable movie to full-screen FX3 video and 16 kHz BRR."""

from __future__ import annotations

import argparse
import array
import json
import math
import struct
import subprocess
import sys
import tempfile
import zlib
from fractions import Fraction
from pathlib import Path

from PIL import Image

WIDTH, HEIGHT = 256, 224
MAP_SIZE, PALETTE_SIZE = 1792, 32
FILE_HEADER = struct.Struct("<4sBBHHHIIIII")
FRAME_HEADER = struct.Struct("<IHBBIIII")


def planar(tile: bytes) -> bytes:
    out = bytearray(32)
    for y in range(8):
        for x in range(8):
            for plane in range(4):
                out[(plane // 2) * 16 + y * 2 + (plane & 1)] |= (
                    ((tile[y * 8 + x] >> plane) & 1) << (7 - x))
    return bytes(out)


def flip(tile: bytes, horizontal: bool, vertical: bool) -> bytes:
    return bytes(tile[(7 - y if vertical else y) * 8 + (7 - x if horizontal else x)]
                 for y in range(8) for x in range(8))


def pack_frame(image: Image.Image) -> tuple[bytes, int]:
    if image.size != (WIDTH, HEIGHT):
        raise ValueError("frames must be 256x224")
    indexed = image.convert("RGB").quantize(colors=16, method=Image.Quantize.MEDIANCUT,
                                          dither=Image.Dither.NONE)
    pixels = indexed.tobytes()
    palette = indexed.getpalette()
    assert palette is not None
    palette += [0] * max(0, 48 - len(palette))
    colors = bytearray()
    for i in range(16):
        r, g, b = palette[i * 3:i * 3 + 3]
        colors += struct.pack("<H", (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
    tiles = bytearray()
    tilemap = bytearray()
    known: dict[bytes, int] = {}
    for ty in range(28):
        for tx in range(32):
            tile = bytes(pixels[(ty * 8 + y) * WIDTH + tx * 8 + x]
                         for y in range(8) for x in range(8))
            if tile not in known:
                number = len(tiles) // 32
                tiles += planar(tile)
                for h, v, bits in ((False, False, 0), (True, False, 0x4000),
                                   (False, True, 0x8000), (True, True, 0xC000)):
                    known.setdefault(flip(tile, h, v), number | bits)
            tilemap += struct.pack("<H", known[tile])
    return bytes(tiles + tilemap + colors), len(tiles) // 32


def packbits(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        run = 1
        while run < 128 and i + run < len(data) and data[i + run] == data[i]:
            run += 1
        if run >= 3:
            out += bytes((257 - run, data[i]))
            i += run
        else:
            start = i
            i += run
            while i < len(data) and i - start < 128:
                run = 1
                while run < 3 and i + run < len(data) and data[i + run] == data[i]:
                    run += 1
                if run == 3:
                    break
                i += min(run, 128 - (i - start))
            out.append(i - start - 1)
            out += data[start:i]
    return bytes(out)


def unpackbits(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        tag = data[i]
        i += 1
        count = tag + 1 if tag < 128 else 257 - tag
        if tag == 128 or i + (count if tag < 128 else 1) > len(data):
            raise ValueError("malformed PackBits")
        if tag < 128:
            out += data[i:i + count]
            i += count
        else:
            out += bytes((data[i],)) * count
            i += 1
    return bytes(out)


def encode_brr(pcm: bytes) -> bytes:
    """Filter-0 BRR; independent blocks remain safe across FIFO underruns."""
    samples = array.array("h")
    samples.frombytes(pcm)
    if sys.byteorder != "little":
        samples.byteswap()
    out = bytearray()
    for start in range(0, len(samples), 16):
        block = list(samples[start:start + 16])
        block += [0] * (16 - len(block))
        best: tuple[int, int, list[int]] | None = None
        for shift in range(13):
            step = 1 << shift
            values = [max(-8, min(7, round(sample / step))) for sample in block]
            error = sum((sample - value * step) ** 2 for sample, value in zip(block, values))
            if best is None or error < best[0]:
                best = error, shift, values
        assert best is not None
        _, shift, values = best
        out.append(shift << 4)  # The SPC receiver owns physical ring END/LOOP bits.
        out += bytes(((values[i] & 15) << 4) | (values[i + 1] & 15)
                     for i in range(0, 16, 2))
    return bytes(out)


def check_budget(payload: bytes, fps: Fraction, refresh: Fraction, budget: int) -> int:
    # Tiles and map may span VBlanks; the final palette/flip is charged too.
    ticks = math.ceil(len(payload) / budget)
    if ticks > math.floor(refresh / fps):
        raise ValueError(f"frame needs {ticks} VBlanks ({len(payload)} DMA bytes); "
                         f"lower --fps to at most {float(refresh / ticks):.3f}")
    return ticks


def convert(source: Path, output: Path, asset: int, fps: Fraction,
            refresh: Fraction, budget: int, silent: bool) -> dict:
    if not 0 < fps <= refresh or not 256 <= budget <= 5120 or not 0 <= asset <= 65535:
        raise ValueError("invalid frame rate, DMA budget (256..5120), or asset ID")
    video = output / "video" / f"{asset:04X}.fmv"
    audio = output / "audio" / f"{asset:04X}.brr"
    video.parent.mkdir(parents=True, exist_ok=True)
    if not silent:
        audio.parent.mkdir(parents=True, exist_ok=True)
    worst = frames = decoded_max = 0
    with tempfile.TemporaryDirectory() as folder:
        records = Path(folder) / "frames.bin"
        with records.open("wb") as dst:
            command = ["ffmpeg", "-v", "error", "-i", str(source), "-an", "-vf",
                       f"fps={fps}:start_time=0,scale=256:224:force_original_aspect_ratio=decrease,"
                       "pad=256:224:(ow-iw)/2:(oh-ih)/2:black", "-f", "rawvideo",
                       "-pix_fmt", "rgb24", "pipe:1"]
            process = subprocess.Popen(command, stdout=subprocess.PIPE)
            assert process.stdout is not None
            try:
                while True:
                    raw = process.stdout.read(WIDTH * HEIGHT * 3)
                    if not raw:
                        break
                    if len(raw) != WIDTH * HEIGHT * 3:
                        raise ValueError("truncated FFmpeg frame")
                    payload, tiles = pack_frame(Image.frombytes("RGB", (WIDTH, HEIGHT), raw))
                    try:
                        ticks = check_budget(payload, fps, refresh, budget)
                    except ValueError as error:
                        raise ValueError(f"frame {frames}: {error}") from error
                    worst = max(worst, ticks)
                    decoded_max = max(decoded_max, len(payload))
                    packed = packbits(payload)
                    codec = int(len(packed) < len(payload))
                    stored = packed if codec else payload
                    pts = round(frames * 1000 / fps)
                    end = round((frames + 1) * 1000 / fps)
                    dst.write(FRAME_HEADER.pack(pts, tiles, codec, 0, len(stored),
                                               len(payload), zlib.crc32(payload), end - pts))
                    dst.write(stored)
                    frames += 1
                if process.wait():
                    raise ValueError("FFmpeg video conversion failed")
            finally:
                process.stdout.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()
        if not frames:
            raise ValueError("source contains no video frames")
        duration = round(frames * 1000 / fps)
        movie = Path(folder) / "movie.fmv"
        with movie.open("wb") as dst, records.open("rb") as src:
            dst.write(FILE_HEADER.pack(b"SFXV", 1, int(not silent), WIDTH, HEIGHT, 32,
                                       frames, duration, 0 if silent else 16000, 0, 0))
            import shutil
            shutil.copyfileobj(src, dst)
        if not silent:
            pcm_path = Path(folder) / "audio.pcm"
            probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0",
                                    "-show_entries", "stream=index", "-of", "csv=p=0",
                                    str(source)], check=True, capture_output=True, text=True)
            command = ["ffmpeg", "-v", "error", "-y"]
            if probe.stdout.strip():
                command += ["-i", str(source), "-map", "0:a:0"]
            else:
                command += ["-f", "lavfi", "-i", "anullsrc=r=16000:cl=mono"]
            command += ["-vn", "-af", "aresample=16000:async=1:first_pts=0,apad", "-t", f"{(duration + 224) / 1000:.3f}",
                        "-ar", "16000", "-ac", "1", "-f", "s16le", str(pcm_path)]
            subprocess.run(command, check=True)
            # Bounded encoding keeps long movies out of the converter's resident memory.
            temporary_audio = Path(folder) / "audio.brr"
            with pcm_path.open("rb") as src, temporary_audio.open("wb") as dst:
                while chunk := src.read(32 * 1024):
                    dst.write(encode_brr(chunk))
            shutil.copyfile(temporary_audio, audio)
        shutil.copyfile(movie, video)
    manifest = {"format": "SFXV1", "asset_id": f"{asset:04X}", "frames": frames,
                "duration_ms": duration, "fps": str(fps), "refresh_hz": str(refresh),
                "dma_budget": budget, "max_dma_bytes": decoded_max,
                "max_upload_vblanks": worst, "video": str(video),
                "audio": None if silent else str(audio)}
    (output / f"{asset:04X}.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--output", type=Path, default=Path("sd"))
    parser.add_argument("--asset-id", type=lambda value: int(value, 16), default=1)
    parser.add_argument("--fps", type=Fraction, default=Fraction(6))
    parser.add_argument("--refresh", type=Fraction, default=Fraction(60))
    parser.add_argument("--dma-budget", type=int, default=3584)
    parser.add_argument("--silent", action="store_true", help="video-only, VBlank clock")
    args = parser.parse_args()
    try:
        print(json.dumps(convert(args.source, args.output, args.asset_id, args.fps,
                                 args.refresh, args.dma_budget, args.silent), indent=2))
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"error: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
