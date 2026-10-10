"""Audio pipeline: VAD segmentation, Whisper speech-to-text, wake-word check, Piper text-to-speech, earcons."""
import os, re, difflib, threading, wave, json
from pathlib import Path

import numpy as np

MIC_RATE = 16000
PLAY_RATE = 22050
WAKE_WORDS = [w.strip().lower() for w in os.environ.get("WAKE_WORDS", "oracle,hey oracle,the oracle").split(",") if w.strip()]


# ----------------------------------------------------------------- VAD segmenter
class Segmenter:
    """Feeds 16 kHz int16 audio through webrtcvad and yields complete utterances."""
    FRAME = 320                 # 20 ms
    PRE_MS, END_MS, MAX_S, MIN_S = 300, 700, 12, 0.35

    def __init__(self):
        import webrtcvad
        self.vad = webrtcvad.Vad(2)
        self.pre = []; self.active = False; self.buf = []; self.silence = 0; self.voiced_recent = []
        self.forced = False      # push-to-talk: capture regardless of VAD until button released + tail
        self.pending = b""

    def feed(self, pcm_bytes, forced=False):
        """Returns a list of finished utterances (np.int16 arrays)."""
        out = []
        self.forced = forced
        data = self.pending + pcm_bytes; n = len(data) // (self.FRAME * 2) * self.FRAME * 2
        self.pending = data[n:]
        for i in range(0, n, self.FRAME * 2):
            f = data[i:i + self.FRAME * 2]
            voiced = self.vad.is_speech(f, MIC_RATE) or forced
            self.voiced_recent = (self.voiced_recent + [voiced])[-5:]
            if not self.active:
                self.pre = (self.pre + [f])[-(self.PRE_MS // 20):]
                if sum(self.voiced_recent) >= 3 or forced:
                    self.active = True; self.buf = list(self.pre); self.silence = 0
            else:
                self.buf.append(f)
                self.silence = 0 if voiced else self.silence + 20
                too_long = len(self.buf) * 20 >= self.MAX_S * 1000
                if (self.silence >= self.END_MS and not forced) or too_long:
                    utt = np.frombuffer(b"".join(self.buf), dtype="<i2")
                    if len(utt) / MIC_RATE >= self.MIN_S + self.END_MS / 1000: out.append(utt)
                    self.active = False; self.buf = []; self.pre = []; self.voiced_recent = []
        return out

    def reset(self):
        self.active = False; self.buf = []; self.pre = []; self.voiced_recent = []; self.pending = b""


# ----------------------------------------------------------------- speech to text
class STT:
    def __init__(self, model=None):
        self.model_name = model or os.environ.get("WHISPER_MODEL", "small")
        self.model = None; self.lock = threading.Lock(); self.err = None

    def load(self):
        from faster_whisper import WhisperModel
        self.model = WhisperModel(self.model_name, device="cpu", compute_type="int8")

    def transcribe(self, pcm: np.ndarray) -> str:
        if self.model is None: self.load()
        audio = pcm.astype(np.float32) / 32768.0
        with self.lock:
            segs, _ = self.model.transcribe(audio, language="en", beam_size=1, vad_filter=False, condition_on_previous_text=False)
            return " ".join(s.text.strip() for s in segs).strip()


# ----------------------------------------------------------------- wake word
def _norm(t): return re.sub(r"[^a-z0-9' ]+", " ", t.lower()).split()

def check_wake(text):
    """Returns (matched, remainder). Fuzzy: tolerates Whisper mishearing 'oracle' slightly."""
    words = _norm(text)
    if not words: return False, ""
    for wake in sorted(WAKE_WORDS, key=len, reverse=True):
        wl = wake.split(); n = len(wl)
        for start in (0, 1):                       # allow one filler word before the wake word ("um oracle")
            cand = words[start:start + n]
            if len(cand) < n: continue
            score = difflib.SequenceMatcher(None, " ".join(cand), wake).ratio()
            if score >= 0.78:
                return True, " ".join(words[start + n:])
    return False, ""


# ----------------------------------------------------------------- text to speech
class TTS:
    def __init__(self, voices_dir: Path):
        self.voice_name = os.environ.get("PIPER_VOICE", "en_US-lessac-medium"); self.voices_dir = voices_dir
        self.voice = None; self.lock = threading.Lock(); self.rate = PLAY_RATE

    def load(self):
        from piper import PiperVoice
        self.voices_dir.mkdir(parents=True, exist_ok=True)
        onnx = self.voices_dir / f"{self.voice_name}.onnx"
        if not onnx.exists():
            import subprocess, sys
            subprocess.run([sys.executable, "-m", "piper.download_voices", self.voice_name, "--data-dir", str(self.voices_dir)], check=True)
        self.voice = PiperVoice.load(str(onnx))
        self.rate = self.voice.config.sample_rate

    def synth(self, text: str) -> np.ndarray:
        """Returns mono int16 at PLAY_RATE."""
        if self.voice is None: self.load()
        with self.lock:
            chunks = []
            if hasattr(self.voice, "synthesize"):
                for ch in self.voice.synthesize(text):
                    chunks.append(np.frombuffer(ch.audio_int16_bytes, dtype="<i2"))
            else:
                for b in self.voice.synthesize_stream_raw(text):
                    chunks.append(np.frombuffer(b, dtype="<i2"))
        pcm = np.concatenate(chunks) if chunks else np.zeros(0, dtype=np.int16)
        if self.rate != PLAY_RATE and len(pcm):
            x = np.arange(len(pcm)); xi = np.arange(0, len(pcm), self.rate / PLAY_RATE)
            pcm = np.interp(xi, x, pcm.astype(np.float64)).astype(np.int16)
        return pcm


# ----------------------------------------------------------------- earcons + sound store
def earcons():
    t = lambda s: np.arange(int(s * PLAY_RATE)) / PLAY_RATE
    def tone(f, s, a=9000): x = np.sin(2 * np.pi * f * t(s)) * a; n = len(x); e = np.ones(n); k = int(0.01 * PLAY_RATE); e[:k] = np.linspace(0, 1, k); e[-k:] = np.linspace(1, 0, k); return x * e
    gap = np.zeros(int(0.04 * PLAY_RATE))
    return {
        "earcon-listen": np.concatenate([tone(660, 0.09), gap, tone(990, 0.12)]),          # rising: I'm listening
        "earcon-done":   np.concatenate([tone(880, 0.07), gap, tone(660, 0.1)]),           # falling: finished
        "earcon-error":  np.concatenate([tone(330, 0.15), gap, tone(330, 0.15)]),
    }

def write_sound(dir_: Path, sid: str, pcm: np.ndarray, meta: dict | None = None):
    d = dir_ / sid; d.mkdir(parents=True, exist_ok=True)
    pcm = np.clip(pcm, -32768, 32767).astype("<i2")
    (d / "pcm.raw").write_bytes(pcm.tobytes())
    w = wave.open(str(d / "preview.wav"), "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(PLAY_RATE); w.writeframes(pcm.tobytes()); w.close()
    (d / "meta.json").write_text(json.dumps({"duration_s": round(len(pcm) / PLAY_RATE, 2), **(meta or {})}))
    return len(pcm) / PLAY_RATE
