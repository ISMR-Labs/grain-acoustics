#!/usr/bin/env python3
"""Plot waveform + spectrogram for every mono WAV in a recording folder and print level stats.

Usage: python3 tools/analysis/spectrogram.py data/recordings/20261010_141142 [out_dir]
"""
import sys, os, json, wave
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from scipy.signal import spectrogram, welch

COLORS = {"mic_A": "#3987e5", "mic_B": "#d95926", "piezo_1": "#199e70", "piezo_2": "#c98500"}

def read(path):
    with wave.open(path) as w:
        rate = w.getframerate(); n = w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64)
    return rate, x

def stats(name, rate, x):
    full = 32768.0 if name.startswith("mic") else 2048.0
    rms = np.sqrt(np.mean(x ** 2)); peak = np.max(np.abs(x))
    f, p = welch(x, rate, nperseg=4096)
    def band(lo, hi):
        m = (f >= lo) & (f < hi); return 10 * np.log10(np.trapezoid(p[m], f[m]) + 1e-12)
    total = 10 * np.log10(np.trapezoid(p, f) + 1e-12)
    hum = p[np.argmin(np.abs(f - 50))] / (np.median(p[(f > 30) & (f < 70)]) + 1e-12)
    return dict(channel=name, rate=rate, seconds=round(len(x) / rate, 1),
                rms_dbfs=round(20 * np.log10(rms / full + 1e-12), 1),
                peak_dbfs=round(20 * np.log10(peak / full + 1e-12), 1),
                clipped_pct=round(100 * np.mean(np.abs(x) >= full - 1), 2),
                energy_below_100hz_pct=round(100 * 10 ** ((band(0, 100) - total) / 10), 1),
                hum_50hz_over_neighbours_db=round(10 * np.log10(hum + 1e-12), 1),
                dc_offset=round(float(np.mean(x)), 1))

def main():
    folder = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else folder
    os.makedirs(out, exist_ok=True)
    names = [n for n in ("mic_A", "mic_B", "piezo_1", "piezo_2") if os.path.exists(f"{folder}/{n}.wav")]
    fig, axes = plt.subplots(len(names), 2, figsize=(14, 2.6 * len(names)), constrained_layout=True)
    report = []
    for i, n in enumerate(names):
        rate, x = read(f"{folder}/{n}.wav"); report.append(stats(n, rate, x))
        t = np.arange(len(x)) / rate
        axes[i, 0].plot(t, x, lw=0.3, color=COLORS[n]); axes[i, 0].set_ylabel(n); axes[i, 0].set_xlim(0, t[-1])
        f, tt, S = spectrogram(x - x.mean(), rate, nperseg=1024, noverlap=768)
        axes[i, 1].pcolormesh(tt, f, 10 * np.log10(S + 1e-9), shading="auto", cmap="magma")
        axes[i, 1].set_ylabel("Hz"); axes[i, 1].set_ylim(0, rate / 2)
    axes[-1, 0].set_xlabel("s"); axes[-1, 1].set_xlabel("s")
    fig.suptitle(os.path.basename(folder.rstrip("/")))
    fig.savefig(f"{out}/{os.path.basename(folder.rstrip('/'))}_overview.png", dpi=110)
    json.dump(report, open(f"{out}/{os.path.basename(folder.rstrip('/'))}_stats.json", "w"), indent=1)
    print(json.dumps(report, indent=1))

if __name__ == "__main__":
    main()
