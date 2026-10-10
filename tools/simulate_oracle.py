"""Fake Oracle device: speaks a phrase (synthesized by the server's own voice) into the mic stream and prints what comes back.
usage: python3 simulate_oracle.py [host:port] ["Oracle, what time is it?"]"""
import http.client, json, struct, sys, time, wave, io
import numpy as np
host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.8:8003"; phrase = sys.argv[2] if len(sys.argv) > 2 else "Oracle, what time is it?"
HDR = struct.Struct("<4sIIIII"); RATE, MF = 16000, 1600
def req(method, path, body=None, headers=None):
    c = http.client.HTTPConnection(*host.split(":")); c.request(method, path, body, headers or {}); r = c.getresponse(); return r.status, r.read()
st, b = req("POST", "/api/tts", json.dumps({"text": phrase}), {"content-type": "application/json"}); sid = json.loads(b)["id"]
st, wav = req("GET", f"/api/sounds/{sid}/wav"); w = wave.open(io.BytesIO(wav)); sr = w.getframerate()
pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
pcm16 = np.interp(np.arange(0, len(pcm), sr / RATE), np.arange(len(pcm)), pcm).astype(np.int16)
print(f"speaking {len(pcm16)/RATE:.1f}s of audio: {phrase!r}")
stream = np.concatenate([np.zeros(RATE, np.int16), pcm16, np.zeros(RATE * 2, np.int16)])
noise = (np.random.default_rng(0).standard_normal(len(stream)) * 20).astype(np.int16); stream = stream + noise
conn = http.client.HTTPConnection(*host.split(":")); seq = 0; t0 = time.time(); playing_until = 0
total = int(len(stream) / MF) + 120   # keep streaming ~12 s after the phrase so the reply can arrive
for seq in range(total):
    chunk = stream[seq * MF:(seq + 1) * MF] if seq * MF < len(stream) else noise[:MF]
    if len(chunk) < MF: chunk = np.concatenate([chunk, np.zeros(MF - len(chunk), np.int16)])
    flags = 1 | (2 if time.time() < playing_until else 0)
    body = HDR.pack(b"AUD1", seq, int((time.time() - t0) * 1000), RATE, MF, flags) + chunk.astype("<i2").tobytes()
    conn.request("POST", "/api/ingest", body, {"Content-Type": "application/octet-stream", "X-Device": "sim-oracle"}); r = conn.getresponse(); resp = r.read()
    if len(resp) > 2:
        print(f"t+{time.time()-t0:4.1f}s command: {resp.decode()}")
        if b'"play"' in resp:
            pid = resp.decode().split('"play":"')[1].split('"')[0]; st, p = req("GET", f"/api/sounds/{pid}/pcm"); dur = len(p) / 2 / 22050
            playing_until = time.time() + dur; print(f"        playing {pid} ({dur:.1f}s)")
    time.sleep(max(0, t0 + (seq + 1) * 0.1 - time.time()))
st, ev = req("GET", "/api/events")
for e in json.loads(ev)[-8:]: print("event:", e["kind"], "-", e.get("text") or e.get("error") or "")
