"""Audio node server: mic stream in (live view + recording), sound library out (played on the ESP32 speaker)."""
import asyncio, json, os, shutil, struct, subprocess, time, uuid, wave
from datetime import datetime
from pathlib import Path

import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, Request, UploadFile, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, Response

HDR = struct.Struct("<4sIIIII")  # magic, seq, uptime_ms, rate, frames, flags
MAGIC = b"AUD1"
PLAY_RATE = 22050
DATA = Path(os.environ.get("DATA_DIR", "data"))
REC_DIR, SND_DIR = DATA / "recordings", DATA / "sounds"
REC_DIR.mkdir(parents=True, exist_ok=True); SND_DIR.mkdir(parents=True, exist_ok=True)
STATIC = Path(__file__).parent / "static"
app = FastAPI(title="Audio Node")


# ----------------------------------------------------------------- device state
class Stats:
    def __init__(self):
        self.device = None; self.last_seen = 0.0; self.last_seq = None; self.chunks = 0; self.gaps = 0
        self.bytes = 0; self.rate = 0; self.uptime_ms = 0; self.flags = 0
        self.kbps = 0.0; self.win_bytes = 0; self.win_t = time.time()
    def online(self): return time.time() - self.last_seen < 3.0
    def snapshot(self):
        now = time.time()
        return {"device": self.device, "online": self.online(), "last_seen_ago": round(now - self.last_seen, 1) if self.last_seen else None,
                "seq": self.last_seq, "chunks": self.chunks, "gaps": self.gaps, "kbps": round(self.kbps, 1), "rate": self.rate,
                "uptime_s": self.uptime_ms // 1000, "playing": bool(self.flags & 2) if self.online() else False}


class Recorder:
    def __init__(self): self.active = False; self.dir = None; self.name = ""; self.started = 0; self.wav = None; self.frames = 0; self.rate = 0; self.gaps = 0
    def start(self, name, rate):
        safe = "".join(ch for ch in name if ch.isalnum() or ch in "-_")[:40]
        rid = datetime.now().strftime("%Y%m%d_%H%M%S") + (f"_{safe}" if safe else "")
        self.dir = REC_DIR / rid; self.dir.mkdir(parents=True, exist_ok=True)
        self.name, self.started, self.rate, self.frames, self.gaps = name, time.time(), rate, 0, 0
        self.wav = wave.open(str(self.dir / "mic.wav"), "wb"); self.wav.setnchannels(1); self.wav.setsampwidth(2); self.wav.setframerate(rate)
        self.active = True; self._meta(False); return rid
    def write(self, pcm, frames, gap):
        if not self.active: return
        if gap: gap = min(gap, 100); self.wav.writeframes(b"\0" * (gap * frames * 2)); self.frames += gap * frames; self.gaps += gap
        self.wav.writeframes(pcm); self.frames += frames
    def stop(self):
        if not self.active: return None
        self.active = False; self.wav.close(); self._meta(True); rid = self.dir.name; self.dir = None; return rid
    def _meta(self, final):
        (self.dir / "meta.json").write_text(json.dumps({"name": self.name, "started": datetime.fromtimestamp(self.started).isoformat(),
            "ended": datetime.now().isoformat() if final else None, "duration_s": round(self.frames / self.rate, 3) if self.rate else 0,
            "file": "mic.wav", "rate": self.rate, "format": "mono int16 = INMP441 24-bit >> 14", "gap_chunks_zero_filled": self.gaps, "complete": final}, indent=1))
    def snapshot(self):
        if not self.active: return {"active": False}
        return {"active": True, "id": self.dir.name, "name": self.name, "duration_s": round(self.frames / self.rate, 1) if self.rate else 0, "bytes": self.frames * 2, "gaps": self.gaps}


stats, rec = Stats(), Recorder()
pending = {}            # next command handed to the device: {"play": id, "vol": v} | {"melody": True} | {"stop": True}
volume = 0.6
now_playing = None      # sound id (or "*melody*") last sent
clients = set()


class Client:
    def __init__(self, ws): self.ws = ws; self.q = asyncio.Queue(maxsize=20)
    def push(self, item):
        if self.q.full():
            try: self.q.get_nowait()
            except asyncio.QueueEmpty: pass
        self.q.put_nowait(item)
    async def run(self):
        while True:
            item = await self.q.get()
            if isinstance(item, bytes): await self.ws.send_bytes(item)
            else: await self.ws.send_text(item)


def status_json():
    return json.dumps({"type": "status", "device": stats.snapshot(), "recording": rec.snapshot(), "volume": volume,
                       "now_playing": now_playing if stats.snapshot()["playing"] else None, "pending": pending, "clients": len(clients)})

def broadcast(item):
    for c in list(clients): c.push(item)


# ----------------------------------------------------------------- sounds library
def sound_meta(d):
    mp = d / "meta.json"
    try: m = json.loads(mp.read_text())
    except Exception: return None
    m["id"] = d.name; return m

def list_sounds():
    out = [m for d in sorted(SND_DIR.iterdir()) if d.is_dir() and (m := sound_meta(d))]
    return sorted(out, key=lambda m: (not m.get("preset"), m.get("name", "").lower()))

def save_pcm(sid, name, pcm: np.ndarray, preset=False, original=None):
    d = SND_DIR / sid; d.mkdir(parents=True, exist_ok=True)
    pcm = np.clip(pcm, -32768, 32767).astype("<i2")
    (d / "pcm.raw").write_bytes(pcm.tobytes())
    w = wave.open(str(d / "preview.wav"), "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(PLAY_RATE); w.writeframes(pcm.tobytes()); w.close()
    (d / "meta.json").write_text(json.dumps({"name": name, "preset": preset, "duration_s": round(len(pcm) / PLAY_RATE, 2),
                                             "bytes": len(pcm) * 2, "rate": PLAY_RATE, "original": original, "added": datetime.now().isoformat()}))

def make_presets():
    t = lambda s: np.arange(int(s * PLAY_RATE)) / PLAY_RATE
    def env(x, a=0.01, r=0.05):
        n = len(x); e = np.ones(n); na, nr = int(a * PLAY_RATE), int(r * PLAY_RATE)
        e[:na] = np.linspace(0, 1, na); e[n - nr:] = np.linspace(1, 0, nr); return x * e
    presets = {
        "preset-beep":   ("Beep (1 kHz, 0.5 s)",  env(np.sin(2 * np.pi * 1000 * t(0.5))) * 12000),
        "preset-double": ("Double beep",           np.concatenate([env(np.sin(2 * np.pi * 1200 * t(0.15))) * 12000, np.zeros(int(0.1 * PLAY_RATE)), env(np.sin(2 * np.pi * 1200 * t(0.15))) * 12000])),
        "preset-alarm":  ("Alarm (two-tone, 3 s)", env(np.sin(2 * np.pi * np.where((t(3) % 0.5) < 0.25, 800, 1000) * t(3))) * 14000),
        "preset-siren":  ("Siren (sweep, 4 s)",    env(np.sin(2 * np.pi * np.cumsum(600 + 600 * (1 + np.sin(2 * np.pi * 0.5 * t(4))) / 2) / PLAY_RATE)) * 14000),
        "preset-chime":  ("Chime (C-E-G)",         np.concatenate([env(np.sin(2 * np.pi * f * t(0.4)) * np.exp(-3 * t(0.4)), 0.005, 0.1) * 12000 for f in (523.25, 659.25, 783.99)])),
        "preset-noise":  ("White noise (2 s)",     env(np.random.default_rng(1).standard_normal(int(2 * PLAY_RATE))) * 5000),
    }
    for sid, (name, pcm) in presets.items():
        if not (SND_DIR / sid / "pcm.raw").exists(): save_pcm(sid, name, pcm, preset=True)

def convert_upload(src: Path) -> np.ndarray:
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", str(src), "-ac", "1", "-ar", str(PLAY_RATE), "-f", "s16le", "-"], capture_output=True)
    if r.returncode != 0 or len(r.stdout) < 4: raise HTTPException(400, "could not decode audio: " + r.stderr.decode(errors="replace")[:200])
    pcm = np.frombuffer(r.stdout, dtype="<i2").astype(np.float64)
    peak = np.abs(pcm).max() or 1; return pcm * (26000 / peak)     # normalise so every sound is similar loudness


# ----------------------------------------------------------------- ingest
@app.post("/api/ingest")
async def ingest(request: Request):
    global pending
    body = await request.body()
    if len(body) < HDR.size: raise HTTPException(400, "short")
    magic, seq, uptime, rate, frames, flags = HDR.unpack_from(body)
    if magic != MAGIC or len(body) != HDR.size + frames * 2: raise HTTPException(400, "bad chunk")
    gap = 0
    if stats.last_seq is not None and seq > stats.last_seq + 1 and seq - stats.last_seq < 10000: gap = seq - stats.last_seq - 1; stats.gaps += gap
    elif stats.last_seq is not None and seq < stats.last_seq: stats.gaps = 0
    stats.device = request.headers.get("x-device", request.client.host); stats.last_seen = time.time(); stats.last_seq = seq
    stats.chunks += 1; stats.bytes += len(body); stats.rate, stats.uptime_ms, stats.flags = rate, uptime, flags
    if rec.active: rec.write(body[HDR.size:], frames, gap)
    if clients: broadcast(body)
    cmd, pending = pending, {}
    return JSONResponse(cmd)


# ----------------------------------------------------------------- api: status / record / recordings
@app.get("/api/status")
async def api_status(): return JSONResponse(json.loads(status_json()))

@app.post("/api/record/start")
async def record_start(request: Request):
    if rec.active: return {"ok": False, "error": "already recording"}
    try: name = str((await request.json()).get("name", ""))[:60]
    except Exception: name = ""
    rid = rec.start(name, stats.rate or 16000); broadcast(status_json()); return {"ok": True, "id": rid}

@app.post("/api/record/stop")
async def record_stop(): rid = rec.stop(); broadcast(status_json()); return {"ok": rid is not None, "id": rid}

@app.get("/api/recordings")
async def api_recordings():
    out = []
    for d in sorted(REC_DIR.iterdir(), reverse=True):
        if not d.is_dir(): continue
        try: m = json.loads((d / "meta.json").read_text())
        except Exception: m = {}
        files = [{"name": f.name, "bytes": f.stat().st_size} for f in sorted(d.iterdir()) if f.is_file()]
        out.append({"id": d.name, "name": m.get("name", ""), "started": m.get("started"), "duration_s": m.get("duration_s", 0),
                    "complete": m.get("complete", False), "gaps": m.get("gap_chunks_zero_filled", 0), "files": files, "bytes": sum(f["bytes"] for f in files)})
    return out

@app.get("/api/recordings/{rid}/{fname}")
async def recording_file(rid: str, fname: str):
    p = (REC_DIR / rid / fname).resolve()
    if REC_DIR.resolve() not in p.parents or not p.is_file(): raise HTTPException(404)
    return FileResponse(p, filename=f"{rid}_{fname}")

@app.delete("/api/recordings/{rid}")
async def recording_delete(rid: str):
    p = (REC_DIR / rid).resolve()
    if p.parent != REC_DIR.resolve() or not p.is_dir(): raise HTTPException(404)
    if rec.active and rec.dir and rec.dir.resolve() == p: raise HTTPException(409, "recording in progress")
    shutil.rmtree(p); return {"ok": True}

@app.post("/api/recordings/{rid}/to-sound")
async def recording_to_sound(rid: str):
    """Copy a mic recording into the sound library so it can be played on the speaker."""
    p = REC_DIR / rid / "mic.wav"
    if not p.is_file(): raise HTTPException(404)
    sid = "rec-" + uuid.uuid4().hex[:8]; save_pcm(sid, f"Recording {rid}", convert_upload(p), original="mic.wav")
    return {"ok": True, "id": sid}


# ----------------------------------------------------------------- api: sounds + playback
@app.get("/api/sounds")
async def api_sounds(): return list_sounds()

@app.post("/api/sounds")
async def upload_sound(file: UploadFile = File(...), name: str = Form("")):
    sid = "snd-" + uuid.uuid4().hex[:8]; d = SND_DIR / sid; d.mkdir(parents=True, exist_ok=True)
    src = d / ("original" + Path(file.filename or "x").suffix.lower()[:8])
    src.write_bytes(await file.read())
    try: pcm = convert_upload(src)
    except HTTPException: shutil.rmtree(d); raise
    save_pcm(sid, name.strip() or Path(file.filename or sid).stem, pcm, original=src.name)
    return {"ok": True, "id": sid, **sound_meta(d)}

@app.get("/api/sounds/{sid}/pcm")
async def sound_pcm(sid: str):
    p = SND_DIR / sid / "pcm.raw"
    if not p.is_file(): raise HTTPException(404)
    return FileResponse(p, media_type="application/octet-stream")

@app.get("/api/sounds/{sid}/wav")
async def sound_wav(sid: str):
    p = SND_DIR / sid / "preview.wav"
    if not p.is_file(): raise HTTPException(404)
    return FileResponse(p, media_type="audio/wav", filename=f"{sid}.wav")

@app.delete("/api/sounds/{sid}")
async def sound_delete(sid: str):
    d = SND_DIR / sid
    if not d.is_dir(): raise HTTPException(404)
    m = sound_meta(d)
    if m and m.get("preset"): raise HTTPException(400, "presets cannot be deleted")
    shutil.rmtree(d); return {"ok": True}

@app.post("/api/play")
async def api_play(request: Request):
    global pending, now_playing, volume
    body = await request.json(); sid = str(body.get("id", ""))
    if "vol" in body: volume = max(0.0, min(1.0, float(body["vol"])))
    if sid == "*melody*": pending = {"melody": True, "vol": volume}
    elif (SND_DIR / sid / "pcm.raw").is_file(): pending = {"play": sid, "vol": volume}
    else: raise HTTPException(404, "no such sound")
    now_playing = sid; broadcast(status_json()); return {"ok": True, "queued": pending}

@app.post("/api/play/stop")
async def api_stop():
    global pending; pending = {"stop": True}; broadcast(status_json()); return {"ok": True}

@app.post("/api/volume")
async def api_volume(request: Request):
    global volume; volume = max(0.0, min(1.0, float((await request.json()).get("vol", volume)))); broadcast(status_json()); return {"ok": True, "vol": volume}


# ----------------------------------------------------------------- websocket / ui
@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept(); c = Client(ws); clients.add(c); sender = asyncio.create_task(c.run())
    try:
        await ws.send_text(status_json())
        while True: await ws.receive_text()
    except Exception: pass
    finally: clients.discard(c); sender.cancel()

async def status_ticker():
    while True:
        await asyncio.sleep(1.0); now = time.time()
        stats.kbps = (stats.bytes - stats.win_bytes) / (now - stats.win_t) / 1000.0; stats.win_bytes, stats.win_t = stats.bytes, now
        if clients: broadcast(status_json())

@app.on_event("startup")
async def _startup():
    make_presets(); asyncio.create_task(status_ticker())

@app.get("/", response_class=HTMLResponse)
async def index(): return (STATIC / "index.html").read_text()
