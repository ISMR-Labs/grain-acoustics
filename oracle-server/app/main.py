"""The Oracle server: mic stream in -> VAD -> Whisper -> wake word -> Claude -> Piper -> speaker on the device."""
import asyncio, difflib, json, os, re, shutil, struct, subprocess, time, uuid
from collections import deque
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import numpy as np
from fastapi import FastAPI, File, Form, HTTPException, Request, UploadFile, WebSocket
from fastapi.responses import FileResponse, HTMLResponse, JSONResponse

from . import brain as brain_mod
from .brain import Brain, MODEL, HA_URL, HA_TOKEN
from .pipeline import STT, TTS, Segmenter, check_wake, earcons, write_sound, PLAY_RATE, WAKE_WORDS

HDR = struct.Struct("<4sIIIII")  # magic, seq, uptime_ms, rate, frames, flags(bit0 mic ok, bit1 playing, bit2 button)
MAGIC = b"AUD1"
DATA = Path(os.environ.get("DATA_DIR", "data")); SND = DATA / "sounds"; SND.mkdir(parents=True, exist_ok=True)
STATIC = Path(__file__).parent / "static"
LISTEN_TIMEOUT, FOLLOWUP_S, PLAY_GUARD_S = 8.0, 6.0, 0.4

app = FastAPI(title="The Oracle")
pool = ThreadPoolExecutor(max_workers=2)
stt, tts, brain = STT(), TTS(DATA / "voices"), Brain()
clients: set["Client"] = set()
volume = 0.25
events = deque(maxlen=200)
ready = {"stt": False, "tts": False, "error": None}


class Device:
    def __init__(self, name):
        self.name = name; self.seg = Segmenter(); self.state = "idle"; self.state_t = time.time()
        self.last_seen = 0.0; self.last_seq = None; self.playing = False; self.play_ended_t = 0.0; self.button = False
        self.pending = {}; self.busy = False; self.chunks = 0; self.uptime = 0; self.kbps = 0.0; self.bytes = 0; self.win = (0, time.time()); self.stats = {}
    def set_state(self, s): self.state, self.state_t = s, time.time(); push_status()
    def snapshot(self):
        online = time.time() - self.last_seen < 3
        return {"name": self.name, "online": online, "state": self.state if online else "offline", "playing": self.playing, "button": self.button,
                "chunks": self.chunks, "uptime_s": self.uptime // 1000, "kbps": round(self.kbps, 1), "last_seen_ago": round(time.time() - self.last_seen, 1) if self.last_seen else None, "stats": self.stats}

devices: dict[str, Device] = {}
def dev(name): return devices.setdefault(name, Device(name))
def main_device(): return max(devices.values(), key=lambda d: d.last_seen) if devices else None


# ----------------------------------------------------------------- web clients
class Client:
    def __init__(self, ws): self.ws = ws; self.q = asyncio.Queue(maxsize=30)
    def push(self, item):
        if self.q.full():
            try: self.q.get_nowait()
            except asyncio.QueueEmpty: pass
        self.q.put_nowait(item)
    async def run(self):
        while True:
            item = await self.q.get()
            await (self.ws.send_bytes(item) if isinstance(item, bytes) else self.ws.send_text(item))

def status_json():
    d = main_device()
    return json.dumps({"type": "status", "device": d.snapshot() if d else {"online": False, "state": "offline"}, "volume": volume,
                       "brain": {"configured": brain.configured(), "model": MODEL, "error": brain.err, "home_assistant": bool(HA_URL and HA_TOKEN)},
                       "stt": {"model": stt.model_name, "ready": ready["stt"]}, "tts": {"voice": tts.voice_name, "ready": ready["tts"]},
                       "wake_words": WAKE_WORDS, "clients": len(clients), "startup_error": ready["error"]})

loop_ref = {"loop": None}
def broadcast(item):
    for c in list(clients): c.push(item)
def push_status():
    loop = loop_ref["loop"]
    if loop: loop.call_soon_threadsafe(broadcast, status_json())
def log(kind, data):
    ev = {"type": "event", "t": time.time(), "kind": kind, **data}; events.append(ev)
    loop = loop_ref["loop"]
    if loop: loop.call_soon_threadsafe(broadcast, json.dumps(ev))


# ----------------------------------------------------------------- speech output
def say(d: Device, text: str, kind="reply"):
    """Synthesize and queue for playback on the device. Runs in a worker thread."""
    pcm = tts.synth(text); sid = "tts-" + uuid.uuid4().hex[:8]
    write_sound(SND, sid, pcm, {"text": text, "kind": kind}); prune_tts()
    d.pending = {"play": sid, "vol": volume}; log("say", {"text": text, "id": sid})
    return sid

def prune_tts(keep=30):
    files = sorted([p for p in SND.iterdir() if p.name.startswith("tts-")], key=lambda p: p.stat().st_mtime)
    for p in files[:-keep]:
        for f in p.iterdir(): f.unlink()
        p.rmdir()

def earcon(d: Device, name):
    d.pending = {"play": name, "vol": volume}



# ----------------------------------------------------------------- music / sound library
def library():
    out = []
    for d in sorted(SND.iterdir()):
        if not d.name.startswith("snd-") or not (d / "meta.json").exists(): continue
        try: m = json.loads((d / "meta.json").read_text()); m["id"] = d.name; out.append(m)
        except Exception: pass
    return sorted(out, key=lambda m: m.get("name", "").lower())

def _tokens(t): return [w for w in re.sub(r"[^a-z0-9 ]+", " ", t.lower()).split() if w not in ("the", "a", "an", "song", "music", "track", "some", "please", "by", "feat", "featuring")]

def find_sound(query):
    """Best fuzzy match of a spoken name against library names/aliases. Returns (meta, score)."""
    q = _tokens(query); best, score = None, 0
    if not q: return None, 0
    for m in library():
        for cand in [m.get("name", "")] + m.get("aliases", []):
            c = _tokens(cand)
            if not c: continue
            overlap = len(set(q) & set(c)) / max(1, min(len(q), len(c)))
            ratio = difflib.SequenceMatcher(None, " ".join(q), " ".join(c)).ratio()
            sc = max(overlap, ratio)
            if sc > score: best, score = m, sc
    return (best, score) if score >= 0.5 else (None, score)

def convert_to_pcm(src: Path):
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", str(src), "-ac", "1", "-ar", str(PLAY_RATE), "-f", "s16le", "-"], capture_output=True)
    if r.returncode != 0 or len(r.stdout) < 4: raise HTTPException(400, "could not decode audio: " + r.stderr.decode(errors="replace")[:200])
    pcm = np.frombuffer(r.stdout, dtype="<i2").astype(np.float64); peak = np.abs(pcm).max() or 1
    return (pcm * (24000 / peak)).astype(np.int16)

def play_sound(d, m):
    d.pending = {"play": m["id"], "vol": volume}; d.set_state("speaking"); log("play", {"text": m["name"], "id": m["id"]})

PLAY_RE = re.compile(r"^(?:(?:hey |ok |okay )?(?:oracle|the oracle)[, ]+)?(?:please |can you |could you )?(?:play|put on|start|playback)\s+(.+?)\s*(?:please)?[.!?]*$", re.I)
STOP_RE = re.compile(r"^(?:(?:oracle|the oracle)[, ]+)?(?:please )?(?:stop|pause|be quiet|shut up|silence|enough)(?:\s+(?:the |that )?(?:music|song|playing|it|playback))?[.!?]*$", re.I)

def music_command(d, text):
    """Handle 'play <song>' / 'stop' locally (works without the Claude key). Returns True if handled."""
    t = text.strip()
    if STOP_RE.match(t): d.pending = {"stop": True}; d.set_state("idle"); log("play", {"text": "stop"}); return True
    mt = PLAY_RE.match(t)
    if mt:
        m, sc = find_sound(mt.group(1))
        if m: play_sound(d, m); return True
        say(d, f"I could not find a song called {mt.group(1)}."); d.set_state("speaking"); return True
    return False

# ----------------------------------------------------------------- the conversation turn (worker thread)
def handle_utterance(d: Device, pcm: np.ndarray, forced: bool):
    try:
        text = stt.transcribe(pcm)
    except Exception as e:
        ready["error"] = f"stt: {e}"; log("error", {"where": "stt", "error": str(e)[:200]}); d.busy = False; return
    dur = round(len(pcm) / 16000, 1)
    if not text:
        log("heard", {"text": "", "dur": dur, "ignored": True}); d.busy = False
        if d.state == "listening": d.set_state("idle"); earcon(d, "earcon-done")
        return
    query = None
    if forced or d.state in ("listening", "followup"):
        query = text
    else:
        matched, rest = check_wake(text)
        if matched:
            log("wake", {"text": text})
            if len(rest.split()) >= 2: query = rest
            else:
                d.set_state("listening"); earcon(d, "earcon-listen"); d.busy = False; return
        else:
            log("heard", {"text": text, "dur": dur, "ignored": True}); d.busy = False; return
    log("query", {"text": query})
    if music_command(d, query): d.busy = False; return
    d.set_state("thinking")
    reply = brain.ask(d.name, query, log)
    log("reply", {"text": reply})
    d.set_state("speaking"); say(d, reply)
    d.busy = False

def ask_text(d: Device, text: str):
    log("query", {"text": text, "typed": True})
    if music_command(d, text): return
    d.set_state("thinking")
    reply = brain.ask(d.name, text, log); log("reply", {"text": reply}); d.set_state("speaking"); say(d, reply)


# ----------------------------------------------------------------- ingest
@app.post("/api/ingest")
async def ingest(request: Request):
    body = await request.body()
    if len(body) < HDR.size: raise HTTPException(400, "short")
    magic, seq, uptime, rate, frames, flags = HDR.unpack_from(body)
    if magic != MAGIC or len(body) != HDR.size + frames * 2: raise HTTPException(400, "bad chunk")
    d = dev(request.headers.get("x-device", request.client.host))
    now = time.time(); d.last_seen = now; d.last_seq = seq; d.chunks += 1; d.uptime = uptime; d.bytes += len(body)
    if (st := request.headers.get("x-stats")): d.stats = dict(kv.split("=", 1) for kv in st.split(",") if "=" in kv)
    if now - d.win[1] >= 1: d.kbps = (d.bytes - d.win[0]) / (now - d.win[1]) / 1000; d.win = (d.bytes, now)
    was_playing = d.playing; d.playing = bool(flags & 2); btn_was = d.button; d.button = bool(flags & 4)
    if d.playing and d.button and not btn_was: d.pending = {"stop": True}; d.set_state("idle"); log("play", {"text": "stop (button)"})
    if was_playing and not d.playing:
        d.play_ended_t = now
        if d.state == "speaking": d.set_state("followup")
    # state timeouts
    if d.state == "listening" and now - d.state_t > LISTEN_TIMEOUT: d.set_state("idle"); earcon(d, "earcon-done")
    if d.state == "followup" and now - d.state_t > FOLLOWUP_S: d.set_state("idle")
    pcm = body[HDR.size:]
    if clients: broadcast(body)
    muted = d.playing or now - d.play_ended_t < PLAY_GUARD_S
    if muted:
        d.seg.reset()
    elif not d.busy and ready["stt"]:
        for utt in d.seg.feed(pcm, forced=d.button):
            d.busy = True
            forced = d.button or d.state in ("listening",)
            pool.submit(handle_utterance, d, utt, d.button)
            break
    cmd, d.pending = d.pending, {}
    if not cmd:
        led = "blink" if d.state == "listening" else "on" if d.state in ("thinking", "speaking", "followup") else "auto"
        if getattr(d, "_led", None) != led: d._led = led; cmd = {"led": led}
    return JSONResponse(cmd)


# ----------------------------------------------------------------- api
@app.get("/api/status")
async def api_status(): return JSONResponse(json.loads(status_json()))

@app.get("/api/events")
async def api_events(): return list(events)

@app.post("/api/ask")
async def api_ask(request: Request):
    d = main_device(); text = str((await request.json()).get("text", "")).strip()
    if not d: raise HTTPException(409, "no device online")
    if not text: raise HTTPException(400, "empty")
    pool.submit(ask_text, d, text); return {"ok": True}

@app.post("/api/say")
async def api_say(request: Request):
    d = main_device(); text = str((await request.json()).get("text", "")).strip()
    if not d: raise HTTPException(409, "no device online")
    pool.submit(say, d, text, "say"); return {"ok": True}

@app.post("/api/stop")
async def api_stop():
    d = main_device()
    if d: d.pending = {"stop": True}; d.set_state("idle")
    return {"ok": True}

@app.post("/api/listen")
async def api_listen():
    """Same as saying the wake word: the next thing heard is the question."""
    d = main_device()
    if not d: raise HTTPException(409, "no device online")
    d.set_state("listening"); earcon(d, "earcon-listen"); return {"ok": True}

@app.post("/api/tone")
async def api_tone():
    """3 s test tone generated on the device itself (no network involved)."""
    d = main_device()
    if not d: raise HTTPException(409, "no device online")
    d.pending = {"play": "*tone*", "vol": volume}; log("play", {"text": "test tone (on-device)"}); return {"ok": True}

@app.post("/api/reset")
async def api_reset():
    d = main_device()
    if d: brain.reset(d.name); d.set_state("idle")
    log("info", {"text": "conversation reset"}); return {"ok": True}

@app.post("/api/volume")
async def api_volume(request: Request):
    global volume; volume = max(0.0, min(1.0, float((await request.json()).get("vol", volume)))); push_status(); return {"ok": True, "vol": volume}


@app.get("/api/library")
async def api_library(): return library()

@app.post("/api/library")
async def api_library_upload(file: UploadFile = File(...), name: str = Form(""), aliases: str = Form("")):
    sid = "snd-" + uuid.uuid4().hex[:8]; d = SND / sid; d.mkdir(parents=True, exist_ok=True)
    src = d / ("original" + Path(file.filename or "x").suffix.lower()[:8]); src.write_bytes(await file.read())
    try: pcm = await asyncio.get_running_loop().run_in_executor(pool, convert_to_pcm, src)
    except HTTPException: shutil.rmtree(d); raise
    nm = name.strip() or re.sub(r"[_\-]+", " ", Path(file.filename or sid).stem)
    write_sound(SND, sid, pcm, {"name": nm, "kind": "music", "aliases": [a.strip() for a in aliases.split(",") if a.strip()], "original": src.name})
    log("info", {"text": f"added to library: {nm}"}); return {"ok": True, "id": sid, "name": nm}

@app.delete("/api/library/{sid}")
async def api_library_delete(sid: str):
    d = SND / sid
    if not sid.startswith("snd-") or not d.is_dir(): raise HTTPException(404)
    shutil.rmtree(d); return {"ok": True}

@app.post("/api/library/{sid}/play")
async def api_library_play(sid: str):
    d = main_device(); m = next((m for m in library() if m["id"] == sid), None)
    if not d: raise HTTPException(409, "no device online")
    if not m: raise HTTPException(404)
    play_sound(d, m); return {"ok": True}

@app.post("/api/tts")
async def api_tts(request: Request):
    """Synthesize text and return a sound id (used by the simulator / tests)."""
    text = str((await request.json()).get("text", "")).strip()
    pcm = await asyncio.get_running_loop().run_in_executor(pool, tts.synth, text)
    sid = "tts-" + uuid.uuid4().hex[:8]; write_sound(SND, sid, pcm, {"text": text}); return {"id": sid}

@app.get("/api/sounds/{sid}/pcm")
async def sound_pcm(sid: str):
    p = SND / sid / "pcm.raw"
    if not p.is_file(): raise HTTPException(404)
    return FileResponse(p, media_type="application/octet-stream")

@app.get("/api/sounds/{sid}/wav")
async def sound_wav(sid: str):
    p = SND / sid / "preview.wav"
    if not p.is_file(): raise HTTPException(404)
    return FileResponse(p, media_type="audio/wav")


# ----------------------------------------------------------------- websocket / startup / ui
@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept(); c = Client(ws); clients.add(c); sender = asyncio.create_task(c.run())
    try:
        await ws.send_text(status_json())
        for ev in list(events)[-50:]: await ws.send_text(json.dumps(ev))
        while True: await ws.receive_text()
    except Exception: pass
    finally: clients.discard(c); sender.cancel()

async def ticker():
    while True:
        await asyncio.sleep(1.0)
        if clients: broadcast(status_json())

def warm_up():
    try:
        for name, pcm in earcons().items(): write_sound(SND, name, pcm, {"kind": "earcon"})
        tts.load(); ready["tts"] = True; log("info", {"text": f"voice ready: {tts.voice_name}"})
        stt.load(); ready["stt"] = True; log("info", {"text": f"speech recognition ready: whisper {stt.model_name}"})
    except Exception as e:
        ready["error"] = str(e)[:300]; log("error", {"where": "startup", "error": str(e)[:300]})
    push_status()

@app.on_event("startup")
async def _startup():
    brain_mod.HOOKS["play"] = lambda dev_name, name: (lambda m: (play_sound(devices[dev_name], m[0]) or f"playing {m[0]['name']}") if m[0] else "no such song")(find_sound(name))
    brain_mod.HOOKS["stop"] = lambda dev_name: (devices[dev_name].pending.update({"stop": True}) or "stopped")
    brain_mod.HOOKS["library"] = lambda: ", ".join(m["name"] for m in library()) or "the library is empty"
    loop_ref["loop"] = asyncio.get_running_loop(); asyncio.create_task(ticker()); pool.submit(warm_up)

@app.get("/", response_class=HTMLResponse)
async def index(): return (STATIC / "index.html").read_text()
