"""Fake audio node: streams a synthetic mic signal (AUD1) and reacts to play commands like the ESP32 would.
usage: python3 simulate_audio_node.py [host:port] [seconds]"""
import http.client, math, random, struct, sys, time
host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.8:8002"; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20
RATE, MF = 16000, 1600; HDR = struct.Struct("<4sIIIII")
conn = http.client.HTTPConnection(*host.split(":")); seq = 0; t0 = time.time(); tm = 0.0; playing_until = 0
while time.time() - t0 < secs:
    mic = []
    for i in range(MF): tm += 1 / RATE; mic.append(int(5000 * math.sin(2 * math.pi * 300 * tm) + random.gauss(0, 60)))
    flags = 1 | (2 if time.time() < playing_until else 0)
    body = HDR.pack(b"AUD1", seq, int((time.time() - t0) * 1000), RATE, MF, flags) + struct.pack(f"<{MF}h", *mic)
    conn.request("POST", "/api/ingest", body, {"Content-Type": "application/octet-stream", "X-Device": "sim-audio"}); r = conn.getresponse(); resp = r.read()
    if len(resp) > 2:
        print("command:", resp.decode())
        if b'"play"' in resp:
            sid = resp.decode().split('"play":"')[1].split('"')[0]
            c2 = http.client.HTTPConnection(*host.split(":")); c2.request("GET", f"/api/sounds/{sid}/pcm"); pr = c2.getresponse(); pcm = pr.read()
            print(f"  fetched pcm {pr.status} {len(pcm)} bytes = {len(pcm)/2/22050:.2f} s"); playing_until = time.time() + len(pcm) / 2 / 22050
        elif b'"melody"' in resp: playing_until = time.time() + 10
    seq += 1; time.sleep(max(0, t0 + seq * 0.1 - time.time()))
print("sent", seq, "chunks")
