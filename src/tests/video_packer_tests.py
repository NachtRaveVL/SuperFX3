#!/usr/bin/env python3
import importlib.util
import math
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from fractions import Fraction
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("video", ROOT / "tools/make_fx3_video.py")
video = importlib.util.module_from_spec(spec)
spec.loader.exec_module(video)


class VideoPackerTests(unittest.TestCase):
    def test_packbits(self):
        randomizer = random.Random(1234)
        for size in (0, 1, 2, 127, 128, 129, 4096, 30496):
            for data in (bytes(size), bytes(randomizer.randrange(256) for _ in range(size))):
                self.assertEqual(video.unpackbits(video.packbits(data)), data)
        for invalid in (b"\x80", b"\xff", b"\x02\x00"):
            with self.assertRaises(ValueError):
                video.unpackbits(invalid)

    def test_planar(self):
        for index in range(16):
            tile = video.planar(bytes((index,)) * 64)
            for plane in range(4):
                for row in range(8):
                    self.assertEqual(tile[(plane // 2) * 16 + row * 2 + (plane & 1)],
                                     255 if index & (1 << plane) else 0)

    def test_dedup_and_flip(self):
        image = Image.new("RGB", (256, 224), "black")
        tile = [(255, 0, 0) if x < y else (0, 0, 0) for y in range(8) for x in range(8)]
        for h, v, xpos in ((False, False, 0), (True, False, 8), (False, True, 16)):
            for y in range(8):
                for x in range(8):
                    image.putpixel((xpos + x, y), tile[(7 - y if v else y) * 8 + (7 - x if h else x)])
        payload, count = video.pack_frame(image)
        self.assertEqual(count, 2)
        entries = struct.unpack_from("<3H", payload, count * 32)
        self.assertEqual(entries[1], entries[0] ^ 0x4000)
        self.assertEqual(entries[2], entries[0] ^ 0x8000)
        self.assertEqual(len(payload), count * 32 + 1792 + 32)

    def test_budget_is_decompressed(self):
        with self.assertRaises(ValueError):
            video.check_budget(bytes(30496), Fraction(20), Fraction(60), 4096)
        self.assertEqual(video.check_budget(bytes(30496), Fraction(6), Fraction(60), 3584), 9)
        with self.assertRaises(ValueError):
            video.check_budget(bytes(30496), Fraction(6), Fraction(50), 3584)

    def test_brr(self):
        data = video.encode_brr(struct.pack("<32h", *([0] * 16 + [1024] * 16)))
        self.assertEqual(len(data), 18)
        self.assertEqual(data[:9], bytes(9))
        self.assertEqual(data[9] & 15, 0)
        self.assertLessEqual(data[9] >> 4, 12)
        self.assertEqual(len(video.encode_brr(struct.pack("<h", -32768))), 9)

    @unittest.skipUnless(shutil.which("ffmpeg") and shutil.which("ffprobe"), "FFmpeg not installed")
    def test_real_movie(self):
        with tempfile.TemporaryDirectory() as folder:
            folder = Path(folder)
            movie = folder / "source.mp4"
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i",
                            "testsrc2=s=256x224:r=30", "-f", "lavfi", "-i",
                            "sine=frequency=440:sample_rate=16000", "-t", "0.4",
                            "-c:v", "mpeg4", "-c:a", "aac", str(movie)], check=True)
            manifest = video.convert(movie, folder, 0x1234, Fraction(6), Fraction(60), 3584, False)
            data = (folder / "video/1234.fmv").read_bytes()
            header = video.FILE_HEADER.unpack_from(data)
            self.assertEqual(header[:6], (b"SFXV", 1, 1, 256, 224, 32))
            offset = 32
            previous = 0
            for _ in range(manifest["frames"]):
                pts, tiles, codec, reserved, stored, decoded, crc, duration = video.FRAME_HEADER.unpack_from(data, offset)
                self.assertEqual((pts, reserved), (previous, 0))
                encoded = data[offset + 24:offset + 24 + stored]
                payload = video.unpackbits(encoded) if codec else encoded
                self.assertEqual(len(payload), decoded)
                self.assertEqual(decoded, tiles * 32 + 1792 + 32)
                self.assertEqual(zlib.crc32(payload), crc)
                previous += duration
                offset += 24 + stored
            self.assertEqual(offset, len(data))
            self.assertEqual(previous, manifest["duration_ms"])
            audio = (folder / "audio/1234.brr").read_bytes()
            self.assertEqual(len(audio), (manifest["duration_ms"] + 224) * 9)
            self.assertTrue(all(not (byte & 15) for byte in audio[::9]))
            native = ROOT / "build/tests/video_stream_tests"
            if native.exists():
                subprocess.run([str(native), str(folder / "video/1234.fmv")], check=True)


if __name__ == "__main__":
    unittest.main()
