# NetProbe — UDP Network Diagnostic Toolkit

A suite of three low-level network measurement tools built from scratch in C using raw socket programming — no `ping`, `iperf3`, or `traceroute` binaries under the hood.

```
netprobe/
├── ping/
│   ├── server.c          # UDP echo server
│   └── client.c          # RTT measurement client
├── iperf/
│   ├── iperf_client.c    # Throughput + delay measurement (threaded)
│   └── plot_stats.py     # Matplotlib visualiser
├── traceroute/
│   └── traceroute.c      # TTL-based hop discovery via raw ICMP
└── Makefile
```

---

## Tools at a Glance

| Tool | Protocol | Socket Type | Measures |
|---|---|---|---|
| **ping** | UDP | `SOCK_DGRAM` | RTT, packet loss |
| **iperf** | UDP | `SOCK_DGRAM` + pthreads | Throughput (Mbps), avg delay per second |
| **traceroute** | UDP + ICMP | `SOCK_DGRAM` + `SOCK_RAW` | Per-hop RTT, path discovery |

---

## Build

```bash
# Build all tools
make all

# Or build individually
make ping
make iperf
make traceroute
```

**Requirements:** GCC, POSIX threads (`-lpthread`), Python 3 + matplotlib (for iperf plots only).

```bash
pip install matplotlib   # for plot_stats.py
```

---

## Tool 1 — ping

> A UDP-based round-trip time measurement tool, similar to the `ping` command.

### How it works

The client embeds a **sequence number** and **microsecond timestamp** directly in the packet payload. The server echoes the packet back byte-for-byte without parsing. The client reads the timestamp out of the returned payload to compute RTT — no clock synchronisation needed.

**Packet format (minimum 12 bytes):**
```
 0        3        11                   N
 +--------+--------+--------------------+
 | seq    |  ts_us |     padding        |
 | uint32 | uint64 |     (0xAB...)      |
 +--------+--------+--------------------+
```

### Usage

```bash
# Terminal 1 — start the server
./ping/server <port>

# Terminal 2 — run the client
./ping/client <server_ip> <port> <num_packets> <interval_ms> <packet_size>
```

**Arguments:**

| Argument | Description |
|---|---|
| `server_ip` | IPv4 address of the server |
| `port` | UDP port (e.g. `9000`) |
| `num_packets` | Total echo packets to send |
| `interval_ms` | Milliseconds between packets |
| `packet_size` | Bytes per packet (minimum `12`) |

### Example

```bash
./ping/server 9000
./ping/client 127.0.0.1 9000 10 1000 64
```

```
Pinging 127.0.0.1:9000 — 10 packets, 1000 ms interval, 64 bytes

seq=0     rtt=0.213 ms  bytes=64
seq=1     rtt=0.198 ms  bytes=64
seq=2     rtt=0.201 ms  bytes=64
...
seq=7     timeout
seq=8     rtt=0.207 ms  bytes=64
seq=9     rtt=0.195 ms  bytes=64

--- 127.0.0.1 ping statistics ---
10 packets transmitted, 9 received, 10.0% packet loss
rtt min/avg/max = 0.195/0.203/0.213 ms
```

> **Timeout behaviour:** The client sets `SO_RCVTIMEO = 2s`. Any packet not returned within that window is counted as lost and printed as `timeout`.

---

## Tool 2 — iperf

> A UDP throughput measurement tool modelled after `iPerf3`. Reports throughput (Mbps) and average round-trip delay (ms) in 1-second intervals.

### How it works

Two POSIX threads run concurrently on a single UDP socket:

```
┌─────────────────────────────────────────────────────┐
│                    iperf_client                      │
│                                                      │
│   Sender Thread                 Receiver Thread      │
│   ─────────────                 ───────────────      │
│   floods UDP packets  ──────►   collects echoes      │
│   at interval_us rate           updates shared       │
│                                 stats (mutex)        │
│                                                      │
│   Main Thread (every 1s)                             │
│   ──────────────────────                             │
│   snapshot + reset window                           │
│   print throughput & delay                          │
│   write row to CSV                                  │
└─────────────────────────────────────────────────────┘
```

**Throughput formula (per 1-second window):**
```
throughput_Mbps = bytes_received × 8 / 1,000,000
avg_delay_ms    = sum(RTT) / packets_received
```

### Usage

```bash
# Start the ping server (iperf reuses it)
./ping/server <port>

# Run the iperf client
./iperf/iperf_client <server_ip> <port> <duration_sec> <packet_size> [interval_us]
```

**Arguments:**

| Argument | Description | Default |
|---|---|---|
| `server_ip` | Server IPv4 address | — |
| `port` | UDP port | — |
| `duration_sec` | How long to run (seconds) | — |
| `packet_size` | Bytes per packet (min `12`) | — |
| `interval_us` | Microseconds between sends | `1000` (1 ms) |

### Example

```bash
./ping/server 9000
./iperf/iperf_client 127.0.0.1 9000 10 512 500
```

```
iPerf-like UDP Test: 127.0.0.1:9000 | 10 sec | 512 bytes/pkt | 500 us interval

Time(s)  Throughput(Mbps)   AvgDelay(ms)     PktsSent     PktsRecv
------   ----------------   ------------     --------     --------
1        47.186             0.312            11482        11492
2        48.204             0.298            11743        11809
3        47.891             0.305            11669        11723
...
```

### Plotting

```bash
python3 iperf/plot_stats.py                        # reads throughput_stats.csv
python3 iperf/plot_stats.py path/to/custom.csv     # custom file
```

Generates `throughput_plot.png` — two stacked charts: **Throughput vs Time** and **Average Delay vs Time**.

![plot example](https://img.shields.io/badge/output-throughput__plot.png-steelblue?style=flat-square)

---

## Tool 3 — traceroute

> Discovers the network path to a destination by sending UDP probes with incrementally increasing TTL values and interpreting ICMP responses.

### How it works

Two sockets are used simultaneously:

```
send_sock  →  SOCK_DGRAM  UDP   port = 33434 + ttl   (sends probes)
recv_sock  →  SOCK_RAW    ICMP                        (receives responses)
```

For each TTL level, the router that drops the packet returns an **ICMP Time Exceeded (type 11)** message. When the destination port is reached, it returns **ICMP Destination Unreachable (type 3)**.

**Probe matching** — the ICMP error payload contains the original IP and UDP headers. The code extracts `inner_ip->ip_dst` and `inner_udp->uh_dport` and verifies both match the current probe, preventing false matches from background traffic.

```
TTL=1  -> Router A  -> ICMP Time Exceeded  (type 11) <- printed
TTL=2  -> Router B  -> ICMP Time Exceeded  (type 11) <- printed
  ...
TTL=N  -> Destination ► ICMP Dest Unreach   (type  3) <- stop
```

A `select()` call with configurable timeout handles non-responding hops, which are printed as `*`.

### Usage

> ⚠️ **Root required** — raw socket creation needs `CAP_NET_RAW`.

```bash
sudo ./traceroute/traceroute <dest_ip> [max_hops] [probes_per_hop] [timeout_ms]
```

**Arguments:**

| Argument | Description | Default |
|---|---|---|
| `dest_ip` | Destination IPv4 address | — |
| `max_hops` | Maximum TTL before giving up | `30` |
| `probes_per_hop` | Probes sent per TTL value | `3` |
| `timeout_ms` | Per-probe receive timeout | `3000` |

### Example

```bash
sudo ./traceroute/traceroute 8.8.8.8
```

```
traceroute to 8.8.8.8, max 30 hops, 3 probes per hop

 1  192.168.1.1    0.412 ms  0.389 ms  0.401 ms
 2  10.0.0.1       3.218 ms  3.104 ms  3.190 ms
 3  *  *  *
 4  72.14.215.165  8.471 ms  8.502 ms  8.489 ms
 5  142.251.49.78  9.102 ms  9.087 ms  9.091 ms
 6  8.8.8.8        9.334 ms  9.318 ms  9.341 ms

Destination 8.8.8.8 reached.
```

---

## Implementation Notes

### Packet Loss Simulation (ping)
To test loss behaviour, you can use `tc` (traffic control) on Linux:
```bash
sudo tc qdisc add dev lo root netem loss 10%    # 10% loss on loopback
sudo tc qdisc del dev lo root                   # remove after testing
```

### Increasing Throughput (iperf)
Decrease `interval_us` to push more packets per second. At `interval_us=0`, the sender runs as fast as the socket allows. Observe how throughput and delay change as the link saturates.

### Why UDP for traceroute?
Standard `traceroute` on Linux uses UDP by default (port 33434+). Since most firewalls don't filter high UDP ports the same way they block ICMP, it reaches more hops. The tool uses port `33434 + ttl` to uniquely match probes to ICMP replies.

---

## File Reference

| File | Purpose |
|---|---|
| `ping/server.c` | UDP echo server — binds, `recvfrom`, `sendto` loop |
| `ping/client.c` | RTT client — timestamp-in-payload, loss tracking |
| `iperf/iperf_client.c` | Threaded sender + receiver, 1s windowed stats, CSV output |
| `iperf/plot_stats.py` | Matplotlib dual-chart visualiser |
| `traceroute/traceroute.c` | Raw ICMP + UDP dual-socket traceroute |
| `Makefile` | Builds all three tools |

---
