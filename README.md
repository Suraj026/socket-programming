# DNS-Assisted UDP Video Streaming System
A three-component system: custom TCP-based DNS server, UDP-based live video streamer, and an integrated client.

---

## System Overview

```
Client --> TCP --> DNS Server     (resolves domain → IP)
Client <-- UDP <-- Video Server   (streams live webcam frames)
```

The client first resolves a domain name using the DNS server over TCP, then uses the returned IP address to receive a live webcam stream over UDP. Video streaming only begins after successful DNS resolution.

---

## Requirements

- Python 3.8 or higher
- A working webcam connected to the machine running `video_server.py`
- All three components run on `localhost` (127.0.0.1) by default

### Setup and Installation

1. **Clone or download the repository:**
```bash
git clone https://github.com/Suraj026/socket-programming.git
cd <directory>
```
*(Alternatively, you can download the project as a ZIP file and extract it)*

2. **Install dependencies:**
```bash
pip install -r requirements.txt
```

Dependencies: `opencv-python`, `numpy`. Everything else uses Python's standard library (`socket`, `struct`, `threading`, `time`, `random`).

---

## Project Structure

```
project/
├── client
    └── client.py          # Client: DNS resolution -> video display
├── records          
    └── dns_records.txt    # Domain -> IP mapping file
├── server        
    └── dns_server.py      # TCP DNS server (port 5000)
    ├── video_server.py    # UDP video streaming server (port 6000)
├── .gitignore              
├── README.md   
├── requirements.txt

```

---

## Running the System

Open **three separate terminal windows** in the project root directory. Start them in this order.

### Terminal 1 — Start the DNS Server

```bash
source venv/bin/activate
python server/dns_server.py
```

Expected output:
```
Loaded 6 record(s) from records/dns_records.txt
Server listening on 0.0.0.0:5000
```

The server listens on TCP port 5000 and handles each client in a separate thread.

---

### Terminal 2 — Start the Video Server

```bash
source venv/bin/activate
python server/video_server.py
```

Expected output:
```
Webcam opened successfully.
Waiting for client on UDP 0.0.0.0:6000 ...
```

The server waits for a `STREAM_START` UDP packet before capturing frames. It will stop streaming automatically 200ms after the client stops sending keep-alive packets.

---

### Terminal 3 — Run the Client

```bash
source venv/bin/activate
python client/client.py
```

Expected output:
```
==================================================
Phase 1: DNS Resolution
==================================================

Domain: youtube.com
IP Address: 127.0.0.1

==================================================
Phase 2: Video Streaming
==================================================
Sent STREAM_START to ('127.0.0.1', 6000)
Press Q in the video window to quit.
```

A window titled **"Video Stream — Press Q to quit"** will open showing the live webcam feed.

**To stop:** Press `Q` in the video window, or close the window. The client sends `STREAM_STOP` to the server, which then stops streaming cleanly.

---

## Protocol Details

### DNS Protocol (TCP, port 5000)

Custom binary protocol — not RFC 1034 compliant.

| Direction | Field | Size | Description |
|---|---|---|---|
| Client -> Server | Transaction ID | 2 bytes | Random uint16, big-endian |
| Client -> Server | Domain Name | N bytes | UTF-8 encoded |
| Client -> Server | Terminator | 1 byte | `\n` (newline) |
| Server -> Client | Transaction ID | 2 bytes | Echoed from request |
| Server -> Client | IP Address | 4 bytes | IPv4 in network byte order |
| Server -> Client | Status | 1 byte | `0x00` = found, `0x01` = NXDOMAIN |

### Video Streaming Protocol (UDP, port 6000)

Each UDP packet carries one fragment of a JPEG-encoded frame.

| Field | Size | Description |
|---|---|---|
| Frame ID | 4 bytes | Monotonically increasing uint32 |
| Total fragments | 4 bytes | How many packets make up this frame |
| Fragment index | 4 bytes | 0-indexed position of this packet |
| JPEG payload | variable | Raw JPEG bytes for this fragment |

Max payload per packet: 60,000 bytes (safe UDP limit avoiding IP fragmentation).

### Keep-Alive (UDP)

- Client sends `b"KEEPALIVE"` to the server every **100ms** via a background thread.
- Server stops streaming if no packet is received for **200ms**.
- Client sends `b"STREAM_STOP"` when the user quits.

---

## Adding or Editing DNS Records

Edit `records/dns_records.txt`. Format: one entry per line.

```
youtube.com 127.0.0.1
my.custom.domain 192.168.1.100
```

Lines starting with `#` are treated as comments. Restart `dns_server.py` after editing the file for changes to take effect.

---

## Changing Ports or Resolution

Edit the configuration constants at the top of each file:

- `dns_server.py` → `start_server(host, port)` call at the bottom
- `video_server.py` → `start_server(host, port)` call at the bottom
- `client.py` → `DNS_SERVER_HOST`, `DNS_SERVER_PORT`, `VIDEO_SERVER_PORT` at the top

---

## Troubleshooting

**`ERROR: Records file not found`**  
Make sure the `records/` folder exists and contains `dns_records.txt`. The file path is relative to where you run the script from — always run from the project root.

**`Could not connect to DNS server`**  
Start `dns_server.py` before running the client.

**`ERROR: Cannot open webcam`**  
Another application may be using the camera. Close it and retry. On Linux, check `ls /dev/video*` to confirm the device exists.

**`OSError: [Errno 98] Address already in use`**  
A previous server instance is still running. Kill it with `Ctrl+C`, wait a few seconds, and restart. Both servers use `SO_REUSEADDR` to minimize this issue.

**Video window appears but is frozen**  
The `cv2.waitKey(1)` call drives the OpenCV GUI event loop. If you see this, it means frames are arriving but not being processed. This should not happen in normal operation — try restarting both the video server and client.

---

No high-level networking frameworks or messaging APIs are used. All network I/O is done via `socket.send()`, `socket.recv()`, `socket.sendto()`, `socket.recvfrom()`.

---
