# Deployment

## Server stacks

Each platform is a Docker Compose stack with a FastAPI app, run as an unprivileged user.

| Stack | Path on server | Container | Port | Hostname |
|---|---|---|---|---|
| sensors | `/opt/stacks/sensors` | `rice-sensors` | 8001 | sensors.dilanp.duckdns.org |
| audio node | `/opt/stacks/audionode` | `audio-node` | 8002 | audio.dilanp.duckdns.org |
| The Oracle | `/opt/stacks/oracle` | `oracle` | 8003 | oracle.dilanp.duckdns.org |

Update a stack:

```bash
scp -r server/Dockerfile server/docker-compose.yml server/requirements.txt server/app user@server:/opt/stacks/sensors/
ssh user@server 'cd /opt/stacks/sensors && docker compose up -d --build'
```

Recordings persist in `/opt/stacks/sensors/recordings` (bind mount). Owner must match the
container user (1000).

## Reverse proxy

Nginx Proxy Manager terminates TLS with a wildcard certificate and forwards to the container
ports with WebSocket support enabled. Create hosts through its UI or API. Split-horizon DNS
(AdGuard) resolves the hostnames to the LAN address inside the lab so the nodes can use the
LAN URL directly, which avoids TLS on the ESP32 and is faster.

## Flashing

```bash
arduino-cli core install esp32:esp32            # 3.3.x
arduino-cli lib install ArduinoOTA WiFiManager   # WiFiManager only for the Oracle
cp firmware/rice_sensors/secrets.h.example firmware/rice_sensors/secrets.h
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/rice_sensors
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/cu.usbserial-XXXX firmware/rice_sensors
```

Over the air after the first USB flash:

```bash
arduino-cli upload --fqbn esp32:esp32:esp32 -p <node ip> --upload-field password=<OTA_PASS> firmware/rice_sensors
```

OTA fails with "No response from device" while the node is busy streaming at full rate; retry,
it normally succeeds on the second attempt.

## Serial monitor gotcha

Most terminal programs assert DTR/RTS when opening the port, which resets the ESP32. To watch
the log without a reboot:

```python
import serial
s = serial.Serial(); s.port = "/dev/cu.usbserial-XXXX"; s.baudrate = 115200
s.dtr = False; s.rts = False; s.open()
```

## Testing without hardware

```bash
python3 tools/simulate_node.py <host:port> 30          # sensors
python3 tools/simulate_audio_node.py <host:port>       # audio node
python3 tools/simulate_oracle.py "Oracle, what time is it?"
```
