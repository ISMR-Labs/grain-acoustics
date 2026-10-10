"""Rice-bag sensor receiver: ingests binary chunks from the ESP32, fans them out to
browsers over WebSocket, and records to WAV on demand."""
import asyncio
import json
import os
import shutil
import struct
import time
import wave
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse, Response

HDR = struct.Struct("<4sIIIIIII")  # magic, seq, uptime_ms, mic_rate, piezo_rate, mic_frames, piezo_frames, flags
MAGIC = b"SNS1"
REC_DIR = Path(os.environ.get("RECORDINGS_DIR", "recordings"))
REC_DIR.mkdir(parents=True, exist_ok=True)
TOKEN = os.environ.get("INGEST_TOKEN", "")
STATIC = Path(__file__).parent / "static"

app = FastAPI(title="Rice Sensors")


# ----------------------------------------------------------------- state
class Stats:
    def __init__(self):
        self.device = None
        self.last_seen = 0.0
        self.last_seq = None
        self.chunks = 0
        self.gaps = 0          # missing chunks detected from seq jumps
        self.bytes = 0
        self.mic_rate = 0
        self.piezo_rate = 0
        self.uptime_ms = 0
        self.flags = 0
        self.win_bytes = 0
        self.win_t = time.time()
        self.kbps = 0.0
        self.chunk_ms = 0

    def online(self):
        return time.time() - self.last_seen < 3.0

    def snapshot(self):
        now = time.time()
        return {
            "device": self.device, "online": self.online(),
            "last_seen_ago": round(now - self.last_seen, 1) if self.last_seen else None,
            "seq": self.last_seq, "chunks": self.chunks, "gaps": self.gaps,
            "kbps": round(self.kbps, 1), "mic_rate": self.mic_rate, "piezo_rate": self.piezo_rate,
            "uptime_s": self.uptime_ms // 1000, "flags": self.flags, "chunk_ms": self.chunk_ms,
        }



def split_channels(d: Path):
    """Write each channel of mics.wav / piezo.wav as its own mono WAV (idempotent)."""
    for src, names in (("mics.wav", ("mic_A.wav", "mic_B.wav")), ("piezo.wav", ("piezo_1.wav", "piezo_2.wav"))):
        sp = d / src
        if not sp.exists() or all((d / n).exists() for n in names): continue
        try:
            w = wave.open(str(sp), "rb"); rate, n = w.getframerate(), w.getnframes()
            data = np.frombuffer(w.readframes(n), dtype="<i2").reshape(-1, 2); w.close()
            for ch, name in enumerate(names):
                o = wave.open(str(d / name), "wb"); o.setnchannels(1); o.setsampwidth(2); o.setframerate(rate)
                o.writeframes(np.ascontiguousarray(data[:, ch]).tobytes()); o.close()
        except Exception as e:
            print("split failed", d, e)

class Recorder:
    def __init__(self):
        self.active = False
        self.dir = None
        self.name = ""
        self.started = 0.0
        self.mic = None
        self.piezo = None
        self.mic_frames = 0
        self.piezo_frames = 0
        self.chunks = 0
        self.gaps = 0
        self.mic_rate = 0
        self.piezo_rate = 0

    def start(self, name, mic_rate, piezo_rate):
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        safe = "".join(ch for ch in name if ch.isalnum() or ch in "-_")[:40]
        rid = f"{stamp}_{safe}" if safe else stamp
        self.dir = REC_DIR / rid
        self.dir.mkdir(parents=True, exist_ok=True)
        self.name, self.started = name, time.time()
        self.mic_rate, self.piezo_rate = mic_rate, piezo_rate
        self.mic = wave.open(str(self.dir / "mics.wav"), "wb")
        self.mic.setnchannels(2); self.mic.setsampwidth(2); self.mic.setframerate(mic_rate)
        self.piezo = wave.open(str(self.dir / "piezo.wav"), "wb")
        self.piezo.setnchannels(2); self.piezo.setsampwidth(2); self.piezo.setframerate(piezo_rate)
        self.mic_frames = self.piezo_frames = self.chunks = self.gaps = 0
        self.active = True
        self._write_meta(final=False)
        return rid

    def write(self, mic_bytes, piezo_u16, mic_frames, piezo_frames, gap_chunks, chunk_ms):
        if not self.active:
            return
        if gap_chunks:
            # keep timing: pad missing chunks with silence
            gap_chunks = min(gap_chunks, 100)
            self.mic.writeframes(b"\0" * (gap_chunks * mic_frames * 4))
            pz = int(self.piezo_rate * chunk_ms / 1000)
            self.piezo.writeframes(b"\0" * (gap_chunks * pz * 4))
            self.mic_frames += gap_chunks * mic_frames
            self.piezo_frames += gap_chunks * pz
            self.gaps += gap_chunks
        self.mic.writeframes(mic_bytes)
        # store raw 12-bit ADC value centred on 2048 so it plays as audio
        self.piezo.writeframes((piezo_u16.astype(np.int32) - 2048).astype("<i2").tobytes())
        self.mic_frames += mic_frames
        self.piezo_frames += piezo_frames
        self.chunks += 1

    def stop(self):
        if not self.active:
            return None
        self.active = False
        self.mic.close(); self.piezo.close()
        self._write_meta(final=True)
        split_channels(self.dir)
        rid = self.dir.name
        self.dir = None
        return rid

    def _write_meta(self, final):
        meta = {
            "name": self.name, "started": datetime.fromtimestamp(self.started).isoformat(),
            "ended": datetime.now().isoformat() if final else None,
            "duration_s": round(self.mic_frames / self.mic_rate, 3) if self.mic_rate else 0,
            "mic": {"file": "mics.wav", "rate": self.mic_rate, "channels": ["mic_A", "mic_B"],
                    "format": "int16 = INMP441 24-bit >> 14", "frames": self.mic_frames},
            "piezo": {"file": "piezo.wav", "rate": self.piezo_rate, "channels": ["piezo_1", "piezo_2"],
                      "format": "int16 = raw 12-bit ADC - 2048 (0 dB atten, 0.55 V bias)", "frames": self.piezo_frames},
            "chunks": self.chunks, "gap_chunks_zero_filled": self.gaps, "complete": final,
            "per_channel_files": {"mic_A.wav": "Mic A mono", "mic_B.wav": "Mic B mono", "piezo_1.wav": "Piezo 1 mono (raw ADC - 2048)", "piezo_2.wav": "Piezo 2 mono (raw ADC - 2048)"},
        }
        (self.dir / "meta.json").write_text(json.dumps(meta, indent=1))

    def snapshot(self):
        if not self.active:
            return {"active": False}
        return {"active": True, "id": self.dir.name, "name": self.name,
                "elapsed_s": round(time.time() - self.started, 1),
                "duration_s": round(self.mic_frames / self.mic_rate, 1) if self.mic_rate else 0,
                "bytes": (self.mic_frames + self.piezo_frames) * 4, "gaps": self.gaps}


stats = Stats()
rec = Recorder()
clients: set["Client"] = set()


class Client:
    """One browser. Chunks are queued; if the browser is slow, old chunks are dropped."""
    def __init__(self, ws):
        self.ws = ws
        self.q = asyncio.Queue(maxsize=20)

    def push(self, item):
        if self.q.full():
            try: self.q.get_nowait()
            except asyncio.QueueEmpty: pass
        self.q.put_nowait(item)

    async def run(self):
        while True:
            item = await self.q.get()
            if isinstance(item, bytes):
                await self.ws.send_bytes(item)
            else:
                await self.ws.send_text(item)


def status_json():
    return json.dumps({"type": "status", "device": stats.snapshot(), "recording": rec.snapshot(),
                       "clients": len(clients), "server_time": time.time()})


def broadcast(item):
    for c in list(clients):
        c.push(item)


# ----------------------------------------------------------------- ingest
@app.post("/api/ingest")
async def ingest(request: Request):
    if TOKEN and request.headers.get("x-token") != TOKEN:
        raise HTTPException(401, "bad token")
    body = await request.body()
    if len(body) < HDR.size:
        raise HTTPException(400, "short")
    magic, seq, uptime, mic_rate, piezo_rate, mic_frames, piezo_frames, flags = HDR.unpack_from(body)
    if magic != MAGIC:
        raise HTTPException(400, "bad magic")
    mic_len = mic_frames * 4
    pz_len = piezo_frames * 4
    if len(body) != HDR.size + mic_len + pz_len:
        raise HTTPException(400, f"length mismatch {len(body)}")

    gap = 0
    if stats.last_seq is not None and seq > stats.last_seq + 1 and seq - stats.last_seq < 10000:
        gap = seq - stats.last_seq - 1
        stats.gaps += gap
    elif stats.last_seq is not None and seq < stats.last_seq:
        stats.gaps = 0          # device rebooted: start the lost-chunk counter over
    stats.device = request.headers.get("x-device", request.client.host)
    stats.last_seen = time.time()
    stats.last_seq = seq
    stats.chunks += 1
    stats.bytes += len(body)
    stats.mic_rate, stats.piezo_rate, stats.uptime_ms, stats.flags = mic_rate, piezo_rate, uptime, flags
    stats.chunk_ms = int(mic_frames * 1000 / mic_rate) if mic_rate else 0

    if rec.active:
        mic_bytes = body[HDR.size:HDR.size + mic_len]
        piezo = np.frombuffer(body, dtype="<u2", count=piezo_frames * 2, offset=HDR.size + mic_len)
        rec.write(mic_bytes, piezo, mic_frames, piezo_frames, gap, stats.chunk_ms)

    if clients:
        broadcast(body)
    return Response(status_code=204)


# ----------------------------------------------------------------- api
@app.get("/api/status")
async def api_status():
    return JSONResponse(json.loads(status_json()))


@app.post("/api/record/start")
async def record_start(request: Request):
    if rec.active:
        return {"ok": False, "error": "already recording", **rec.snapshot()}
    try:
        payload = await request.json()
    except Exception:
        payload = {}
    name = str(payload.get("name", ""))[:60]
    rid = rec.start(name, stats.mic_rate or 16000, stats.piezo_rate or 10000)
    broadcast(status_json())
    return {"ok": True, "id": rid, **rec.snapshot()}


@app.post("/api/record/stop")
async def record_stop():
    rid = rec.stop()
    broadcast(status_json())
    return {"ok": rid is not None, "id": rid}


def list_recordings():
    out = []
    for d in sorted(REC_DIR.iterdir(), reverse=True):
        if not d.is_dir():
            continue
        meta = {}
        mp = d / "meta.json"
        if mp.exists():
            try: meta = json.loads(mp.read_text())
            except Exception: pass
        if meta.get("complete"): split_channels(d)
        order = {"mic_A.wav": 0, "mic_B.wav": 1, "piezo_1.wav": 2, "piezo_2.wav": 3, "mics.wav": 4, "piezo.wav": 5, "meta.json": 6}
        files = [{"name": f.name, "bytes": f.stat().st_size} for f in sorted((f for f in d.iterdir() if f.is_file()), key=lambda f: (order.get(f.name, 9), f.name))]
        out.append({"id": d.name, "name": meta.get("name", ""), "started": meta.get("started"),
                    "duration_s": meta.get("duration_s", 0), "complete": meta.get("complete", False),
                    "gaps": meta.get("gap_chunks_zero_filled", 0), "files": files,
                    "bytes": sum(f["bytes"] for f in files)})
    return out


@app.get("/api/recordings")
async def api_recordings():
    return list_recordings()


@app.get("/api/recordings/{rid}/{fname}")
async def api_recording_file(rid: str, fname: str):
    p = (REC_DIR / rid / fname).resolve()
    if REC_DIR.resolve() not in p.parents or not p.is_file():
        raise HTTPException(404)
    return FileResponse(p, filename=f"{rid}_{fname}")


@app.delete("/api/recordings/{rid}")
async def api_recording_delete(rid: str):
    p = (REC_DIR / rid).resolve()
    if p.parent != REC_DIR.resolve() or not p.is_dir():
        raise HTTPException(404)
    if rec.active and rec.dir and rec.dir.resolve() == p:
        raise HTTPException(409, "recording in progress")
    shutil.rmtree(p)
    return {"ok": True}


# ----------------------------------------------------------------- websocket
@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    c = Client(ws)
    clients.add(c)
    sender = asyncio.create_task(c.run())
    try:
        await ws.send_text(status_json())
        while True:
            msg = await ws.receive_text()   # browser may send pings; ignore content
            if msg == "status":
                c.push(status_json())
    except WebSocketDisconnect:
        pass
    except Exception:
        pass
    finally:
        clients.discard(c)
        sender.cancel()


async def status_ticker():
    while True:
        await asyncio.sleep(1.0)
        now = time.time()
        stats.kbps = (stats.bytes - stats.win_bytes) / (now - stats.win_t) / 1000.0
        stats.win_bytes, stats.win_t = stats.bytes, now
        if clients:
            broadcast(status_json())


@app.on_event("startup")
async def _startup():
    asyncio.create_task(status_ticker())


@app.get("/", response_class=HTMLResponse)
async def index():
    return (STATIC / "index.html").read_text()
