"""Fake ESP32: streams synthetic SNS1 chunks to the server (for testing without hardware).
usage: python3 simulate_node.py [host:port] [seconds]"""
import http.client, math, random, struct, sys, time
host = sys.argv[1] if len(sys.argv) > 1 else "192.168.1.8:8001"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30
MIC_RATE, PZ_RATE, CHUNK_MS = 16000, 10000, 100
MF, PF = MIC_RATE * CHUNK_MS // 1000, PZ_RATE * CHUNK_MS // 1000
HDR = struct.Struct("<4sIIIIIII")
conn = http.client.HTTPConnection(*host.split(":")) if ":" in host else http.client.HTTPConnection(host)
seq, t0, sent = 0, time.time(), 0
tm = tp = 0.0
while time.time() - t0 < secs:
    mic = []
    for i in range(MF):
        tm += 1 / MIC_RATE
        mic.append(int(6000 * math.sin(2 * math.pi * 440 * tm) + random.gauss(0, 80)))
        mic.append(int(3000 * math.sin(2 * math.pi * 1200 * tm) + random.gauss(0, 80)))
    pz = []
    for i in range(PF):
        tp += 1 / PZ_RATE
        burst = 40 * math.exp(-((tp % 1.0) / 0.01)) * math.sin(2 * math.pi * 2500 * tp)   # click every second
        pz.append(max(0, min(4095, int(2050 + burst + random.gauss(0, 1.5)))))
        pz.append(max(0, min(4095, int(2040 + random.gauss(0, 1.5)))))
    body = HDR.pack(b"SNS1", seq, int((time.time() - t0) * 1000), MIC_RATE, PZ_RATE, MF, PF, 3)
    body += struct.pack(f"<{MF*2}h", *mic) + struct.pack(f"<{PF*2}H", *pz)
    conn.request("POST", "/api/ingest", body, {"Content-Type": "application/octet-stream", "X-Device": "simulator"})
    r = conn.getresponse(); r.read()
    if r.status not in (200, 204): print("HTTP", r.status)
    seq += 1; sent += len(body)
    time.sleep(max(0, t0 + seq * CHUNK_MS / 1000 - time.time()))
print(f"sent {seq} chunks, {sent/1000:.0f} kB in {time.time()-t0:.1f}s")
