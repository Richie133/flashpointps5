#!/usr/bin/env python3
# Flashpoint PS5 - renders the app's intro music and interface sounds.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: python3 tools/make-audio.py [assets/audio]
#
# Everything is synthesised here from oscillators and noise (an original
# composition, no samples), so the WAV files in assets/audio can be rebuilt
# exactly. 48 kHz, 16-bit: the intro in stereo, the interface sounds mono.

import sys
import wave
from pathlib import Path

import numpy as np

RATE = 48000
OUT = Path(sys.argv[1] if len(sys.argv) > 1 else "assets/audio")
rng = np.random.default_rng(1996)


def note(name):
    """'A4' -> Hz."""
    names = {"C": 0, "C#": 1, "D": 2, "D#": 3, "E": 4, "F": 5, "F#": 6, "G": 7, "G#": 8, "A": 9, "A#": 10, "B": 11}
    pitch, octave = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((names[pitch] + 12 * (octave + 1) - 69) / 12)


def t_axis(seconds):
    return np.arange(int(seconds * RATE)) / RATE


def env(n, attack, decay, sustain=0.0, release=0.0, hold=None):
    """ADSR over n samples (times in seconds)."""
    a, d, r = int(attack * RATE), int(decay * RATE), int(release * RATE)
    e = np.full(n, sustain, dtype=np.float64)
    a = min(a, n)
    e[:a] = np.linspace(0, 1, a, endpoint=False) if a else e[:a]
    d_end = min(n, a + d)
    if d_end > a:
        e[a:d_end] = np.linspace(1, sustain, d_end - a, endpoint=False)
    if r:
        r = min(r, n)
        e[n - r:] *= np.linspace(1, 0, r)
    return e


def pulse(f, t, width=0.5):
    ph = (f * t) % 1.0
    return np.where(ph < width, 1.0, -1.0)


def saw(f, t):
    return 2.0 * ((f * t) % 1.0) - 1.0


def tri(f, t):
    return 2.0 * np.abs(2.0 * ((f * t) % 1.0) - 1.0) - 1.0


def lowpass(x, cutoff):
    """One-pole low-pass; cutoff may be an array (a sweep)."""
    cutoff = np.broadcast_to(np.asarray(cutoff, dtype=np.float64), x.shape)
    a = np.exp(-2 * np.pi * cutoff / RATE)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a[i]) * x[i] + a[i] * acc
        y[i] = acc
    return y


def reverb(x, mix=0.25, room=0.82):
    """A small Schroeder reverb (four combs, two all-passes)."""
    out = np.zeros_like(x)
    for delay in (1557, 1617, 1491, 1422):
        buf = np.zeros(len(x) + delay)
        y = np.zeros_like(x)
        for i in range(len(x)):
            y[i] = x[i] + room * buf[i]
            buf[i + delay] = y[i]
        out += y
    out /= 4
    for delay, g in ((225, 0.5), (556, 0.5)):
        y = np.zeros_like(out)
        buf = np.zeros(len(out) + delay)
        for i in range(len(out)):
            v = buf[i]
            y[i] = -g * out[i] + v
            buf[i + delay] = out[i] + g * y[i]
        out = y
    return (1 - mix) * x + mix * out


def place(track, sound, at):
    i = int(at * RATE)
    end = min(len(track), i + len(sound))
    track[i:end] += sound[: end - i]


def write(path, data, channels):
    peak = np.max(np.abs(data)) or 1.0
    data = data / peak * 0.89
    pcm = (np.clip(data, -1, 1) * 32767).astype("<i2")
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())
    print(f"{path}: {len(pcm) // channels / RATE:.2f} s")


# ------------------------------------------------------------------ intro
# 124 bpm, four bars (D - Bm - G - A), then a held D with a sparkle.
BPM = 124
BEAT = 60 / BPM
BAR = 4 * BEAT
LENGTH = 4 * BAR + 2.6
left = np.zeros(int(LENGTH * RATE))
right = np.zeros_like(left)

chords = [("D3", ["D4", "F#4", "A4"]), ("B2", ["B3", "D4", "F#4"]),
          ("G2", ["G3", "B3", "D4"]), ("A2", ["A3", "C#4", "E4"])]

# pad: detuned saws, filter opening over the four bars
for bar, (_, tones) in enumerate(chords):
    t = t_axis(BAR + 0.35)
    pad = np.zeros_like(t)
    for name in tones:
        f = note(name)
        pad += saw(f * 0.997, t) + saw(f * 1.003, t)
    pad = lowpass(pad, 500 + 900 * bar + 600 * t / BAR) * env(len(t), 0.25, 0.4, 0.75, 0.35) * 0.06
    place(left, pad, bar * BAR)
    place(right, pad, bar * BAR)

# bass: triangle on the beat, octave jump on the "and" of beat four
for bar, (root, _) in enumerate(chords):
    for beat in range(4):
        f = note(root)
        for off, mult in ((0.0, 1), (0.5, 2 if beat == 3 else 1)):
            t = t_axis(BEAT * 0.45)
            b = tri(f * mult, t) * env(len(t), 0.004, 0.18, 0.3, 0.05) * 0.32
            at = bar * BAR + (beat + off) * BEAT
            place(left, b, at)
            place(right, b, at)

# arpeggio: plucked pulses in sixteenths, panned left/right
for bar, (_, tones) in enumerate(chords):
    seq = tones + [tones[1].replace("4", "5").replace("3", "4")]
    for step in range(16):
        name = seq[step % 4]
        f = note(name) * 2
        t = t_axis(BEAT / 4 * 0.95)
        p = pulse(f, t, 0.25) * env(len(t), 0.002, 0.09, 0.0) * 0.05
        p = lowpass(p, 3500)
        at = bar * BAR + step * BEAT / 4
        pan = 0.5 + 0.35 * np.sin(step * 0.8)
        place(left, p * (1 - pan) * 2, at)
        place(right, p * pan * 2, at)

# lead: the motif, square wave with a little vibrato
melody = [("A4", 0, 1), ("D5", 1, 1), ("F#5", 2, 1.5), ("E5", 3.5, 0.5),
          ("D5", 4, 1), ("F#5", 5, 1), ("B5", 6, 2),
          ("D5", 8, 1), ("G5", 9, 1), ("B5", 10, 1.5), ("A5", 11.5, 0.5),
          ("G5", 12, 1), ("A5", 13, 0.5), ("C#6", 13.5, 0.5), ("E6", 14, 2)]
for name, start, beats in melody:
    t = t_axis(beats * BEAT)
    f = note(name) * (1 + 0.004 * np.sin(2 * np.pi * 5.5 * t) * np.minimum(1, t * 3))
    phase = np.cumsum(f) / RATE
    sq = np.where(phase % 1.0 < 0.5, 1.0, -1.0)
    lead = lowpass(sq, 2600) * env(len(t), 0.01, 0.12, 0.6, 0.06) * 0.11
    place(left, lead, start * BEAT)
    place(right, lead, start * BEAT + 0.012)

# drums: soft kick on every beat, hats on the off-beats, a noise swell in
for i in range(16):
    t = t_axis(0.22)
    kick = np.sin(2 * np.pi * (45 + 110 * np.exp(-t * 28)) * t) * np.exp(-t * 14) * 0.36
    place(left, kick, i * BEAT)
    place(right, kick, i * BEAT)
    hat = lowpass(rng.standard_normal(int(0.05 * RATE)), 9000)
    hat = (rng.standard_normal(len(hat)) - hat) * env(len(hat), 0.001, 0.04) * 0.035
    place(left, hat * 0.8, (i + 0.5) * BEAT)
    place(right, hat * 1.2, (i + 0.5) * BEAT)
swell_t = t_axis(BAR)
swell = lowpass(rng.standard_normal(len(swell_t)), 600 + 6000 * swell_t / BAR) * (swell_t / BAR) ** 2 * 0.08
place(left, swell, 3 * BAR)
place(right, swell, 3 * BAR)

# ending: the D chord held, plus a rising sparkle
end_t = t_axis(2.6)
final = np.zeros_like(end_t)
for name in ("D3", "A3", "D4", "F#4", "A4", "D5"):
    f = note(name)
    final += (saw(f * 0.998, end_t) + saw(f * 1.002, end_t)) * 0.5
final = lowpass(final, 2200 * np.exp(-end_t * 0.9) + 300) * env(len(end_t), 0.01, 0.3, 0.55, 1.6) * 0.17
place(left, final, 4 * BAR)
place(right, final, 4 * BAR)
for k, name in enumerate(("D6", "F#6", "A6", "D7")):
    t = t_axis(0.9)
    s = np.sin(2 * np.pi * note(name) * t) * np.exp(-t * 4.5) * 0.1
    place(left if k % 2 else right, s, 4 * BAR + 0.08 * k)

mix_l, mix_r = reverb(left, 0.22), reverb(right, 0.22)
fade = np.ones_like(mix_l)
fade[-int(0.4 * RATE):] = np.linspace(1, 0, int(0.4 * RATE))
stereo = np.empty(len(mix_l) * 2)
stereo[0::2] = mix_l * fade
stereo[1::2] = mix_r * fade
write(OUT / "intro.wav", stereo, 2)

# ------------------------------------------------------- interface sounds


def blip(freqs, step, length, shape=np.sin, gain=1.0, decay=30.0):
    total = int((step * (len(freqs) - 1) + length) * RATE)
    out = np.zeros(total)
    for i, f in enumerate(freqs):
        t = t_axis(length)
        tone = shape(2 * np.pi * f * t) if shape is np.sin else shape(f, t)
        place(out, tone * np.exp(-t * decay) * env(len(t), 0.002, 0.0, 1.0, 0.01) * gain, i * step)
    return out


def tick():
    t = t_axis(0.035)
    return np.sin(2 * np.pi * 2100 * t) * np.exp(-t * 140) * 0.6 + \
        lowpass(rng.standard_normal(len(t)), 5000) * np.exp(-t * 300) * 0.25


write(OUT / "move.wav", tick(), 1)
write(OUT / "select.wav", blip([note("E6"), note("B6")], 0.045, 0.11, decay=26), 1)
write(OUT / "back.wav", blip([note("B5"), note("E5")], 0.045, 0.11, decay=26), 1)
write(OUT / "tab.wav", blip([note("A5")], 0, 0.07, shape=tri, gain=0.7, decay=45), 1)
write(OUT / "play.wav", reverb(blip([note("D5"), note("F#5"), note("A5"), note("D6")], 0.055, 0.28,
                                    shape=lambda f, t: pulse(f, t, 0.25) * 0.5, decay=9), 0.25), 1)
write(OUT / "error.wav", blip([note("A2"), note("A2")], 0.11, 0.09,
                              shape=lambda f, t: lowpass(pulse(f, t, 0.5), 900), decay=12), 1)
write(OUT / "open.wav", blip([note("D5"), note("A5")], 0.035, 0.09, decay=30), 1)
