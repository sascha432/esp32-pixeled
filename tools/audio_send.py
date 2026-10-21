#!/usr/bin/env python3
"""Sends audio packets to a device's UDP inputs (docs/leds.md section 6.1): WARLS spectrum packets
(the kfc_fw format) or WLED audio sync v2, so the reactive animations can be tried without a
microphone.

    python tools/audio_send.py <device>                      WARLS to <device>:21324, a demo signal
    python tools/audio_send.py <device> --wled               WLED audio sync to <device>:11988
    python tools/audio_send.py --wled                        WLED audio sync to 239.0.0.1:11988
    python tools/audio_send.py <device> --input              the PC's sound input (numpy, sounddevice)
    python tools/audio_send.py <device> --seconds 20 --fps 40

The device needs the input switched on (LEDs page, card "Sound input", or the console `audio set
warls on` / `audio set wled on`). The demo is 120 beats a minute: a kick in the bass on every beat,
a melody running through the middle bands, a hi-hat on the off beats. On exit a silent packet goes
out, so the device falls back at once. Only the Python standard library for the demo.
"""

import argparse
import math
import random
import socket
import struct
import sys
import time

BANDS = 32
WARLS_PORT = 21324
WLED_PORT = 11988
WLED_GROUP = "239.0.0.1"


def warls_packet(bands: list[int], left: int, right: int, timeout: int = 2) -> bytes:
    """protocol 1, timeout s, magic 0x4dde (LE), left, right, magic 0x42, count, bands, padding."""
    count = len(bands)
    packet = bytearray(2 + 4 + 2 + ((count + 3) & ~3) + 2)
    packet[0:8] = bytes([1, timeout, 0xDE, 0x4D, left, right, 0x42, count])
    packet[8:8 + count] = bytes(bands)
    return bytes(packet)


def wled_packet(bands: list[int], loudness: float, beat: bool) -> bytes:
    """WLED audio sync v2, 44 bytes: header, sampleRaw, sampleSmth, samplePeak, 16 GEQ bands,
    FFT magnitude and major peak."""
    geq = [max(bands[i * 2], bands[i * 2 + 1]) for i in range(16)]
    return struct.pack("<6s2xffBB16sHff", b"00002\0", loudness, loudness, 1 if beat else 0, 0, bytes(geq), 0,
                       loudness * 16.0, 440.0)


def demo(t: float) -> tuple[list[int], int, bool]:
    """The demo signal at `t` seconds: 32 bands, the loudness, whether a beat starts."""
    beat_length = 0.5
    phase = (t % beat_length) / beat_length
    kick = math.exp(-phase * 8.0)
    hat = math.exp(-((phase + 0.5) % 1.0) * 14.0)
    melody = 10 + 8 * math.sin(t * 1.3) + 4 * math.sin(t * 3.1)
    bands = []
    for b in range(BANDS):
        value = 30 + 15 * math.sin(t * 2 + b * 0.4)                        # a floor that moves
        value += 220 * kick * max(0.0, 1 - b / 5)                          # the kick: bands 0 to 4
        value += 180 * math.exp(-((b - melody) ** 2) / 6)                  # the melody
        value += 160 * hat * max(0.0, (b - 22) / 9)                        # the hi-hat: the treble
        value += random.uniform(-8, 8)
        bands.append(int(max(0, min(255, value))))
    loudness = int(max(0, min(255, 90 + 120 * kick + 30 * math.sin(t * 0.7))))
    return bands, loudness, phase < 1.0 / 40


class Microphone:
    """The PC's default sound input: 32 log bands from 50 Hz to 10 kHz, scaled to the loudest of
    the last seconds (an automatic gain like the device's)."""

    def __init__(self):
        import numpy
        import sounddevice
        self.np = numpy
        self.rate = 44100
        self.size = 2048
        self.block = numpy.zeros(self.size, dtype=numpy.float32)
        self.stream = sounddevice.InputStream(channels=1, samplerate=self.rate, blocksize=1024,
                                              callback=self._take)
        self.stream.start()
        edges = numpy.geomspace(50, 10000, BANDS + 1)
        self.bins = [(int(edges[i] * self.size / self.rate), max(int(edges[i + 1] * self.size / self.rate),
                      int(edges[i] * self.size / self.rate) + 1)) for i in range(BANDS)]
        self.window = numpy.hanning(self.size)
        self.reference = -60.0
        self.bass = 0.0
        self.last_beat = 0.0

    def _take(self, data, frames, time_info, status):
        self.block = self.np.roll(self.block, -frames)
        self.block[-frames:] = data[:, 0]

    def read(self, t: float) -> tuple[list[int], int, bool]:
        np = self.np
        spectrum = np.abs(np.fft.rfft(self.block * self.window))
        db = [20 * math.log10(max(float(spectrum[a:b].max()), 1e-9)) for a, b in self.bins]
        loudest = max(db)
        self.reference = max(loudest, self.reference - 0.1)
        bands = [int(max(0, min(255, (v - (self.reference - 40)) / 40 * 255))) for v in db]
        loudness = int(max(0, min(255, sum(bands) / BANDS * 1.5)))
        bass = sum(bands[:4])
        beat = bass > self.bass * 1.6 and bass > 200 and t - self.last_beat > 0.14
        self.last_beat = t if beat else self.last_beat
        self.bass += (bass - self.bass) * 0.1
        return bands, loudness, beat


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("device", nargs="?", help="the device's address; WLED sync without it: multicast")
    parser.add_argument("--wled", action="store_true", help="WLED audio sync v2 instead of WARLS")
    parser.add_argument("--port", type=int, help=f"the port (WARLS {WARLS_PORT}, WLED {WLED_PORT})")
    parser.add_argument("--fps", type=float, default=40, help="packets a second (40)")
    parser.add_argument("--seconds", type=float, default=0, help="stop after this long (0: Ctrl+C)")
    parser.add_argument("--input", action="store_true", help="the PC's sound input instead of the demo")
    args = parser.parse_args()
    if not args.wled and not args.device:
        parser.error("WARLS needs the device's address")
    host = args.device or WLED_GROUP
    port = args.port or (WLED_PORT if args.wled else WARLS_PORT)

    source = Microphone() if args.input else None
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    out.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    print(f"{'WLED audio sync' if args.wled else 'WARLS'} to {host}:{port}, {args.fps:g} a second, "
          f"{'the sound input' if source else 'the demo'}; Ctrl+C stops")
    start = time.monotonic()
    sent = 0
    try:
        while args.seconds <= 0 or time.monotonic() - start < args.seconds:
            t = time.monotonic() - start
            bands, loudness, beat = source.read(t) if source else demo(t)
            packet = wled_packet(bands, float(loudness), beat) if args.wled else warls_packet(bands, loudness, loudness)
            out.sendto(packet, (host, port))
            sent += 1
            if sent % int(max(1, args.fps)) == 0:
                bars = "".join(" .:-=+*#%@"[min(9, v * 10 // 256)] for v in bands)
                print(f"\r{sent:6d} sent  loudness {loudness:3d} {'BEAT' if beat else '    '} |{bars}|", end="", flush=True)
            time.sleep(max(0.0, start + sent / args.fps - time.monotonic()))
    except KeyboardInterrupt:
        pass
    finally:
        silent = [0] * BANDS
        out.sendto(wled_packet(silent, 0.0, False) if args.wled else warls_packet(silent, 0, 0, 1), (host, port))
        print(f"\n{sent} packets sent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
