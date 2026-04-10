# Client Application
#
# Phase 1 — DNS Resolution (TCP):
#   Sends a DNS query to the DNS server and receives the resolved IP address.
#   Packet format:
#     Request:  [ txn_id (2B) ][ domain (UTF-8) ][ '\n' (1B) ]
#     Response: [ txn_id (2B) ][ ip (4B) ][ status (1B) ]
#
# Phase 2 — Video Streaming (UDP):
#   Uses the resolved IP to connect to the video server.
#   Receives JPEG frame fragments, reassembles them, and displays via OpenCV.
#   Sends b"KEEPALIVE" every 100ms on a background thread.
#   Press Q in the video window to quit cleanly.

import socket
import struct
import threading
import time
import random
import numpy as np
import cv2


# Configuration 

DNS_SERVER_HOST = "127.0.0.1"
DNS_SERVER_PORT = 5000

VIDEO_SERVER_PORT = 6000

DOMAIN_TO_RESOLVE = "youtube.com"

# Keep-alive interval in seconds (must be well below the server's 200ms timeout)
KEEPALIVE_INTERVAL = 0.100   # 100 ms

# How long to wait for a UDP frame packet before looping (non-blocking trick)
UDP_RECV_TIMEOUT = 2.0

# Stale frame cleanup: drop incomplete frames older than this many seconds
FRAME_STALE_TIMEOUT = 0.5


# Phase 1: DNS Resolution 

def resolve_domain(domain):
    """
    Perform a custom DNS lookup over TCP.
    Returns the resolved IP address string, or raises an exception on failure.
    """
    txn_id = random.randint(0, 65535)

    # Build request: 2-byte txn_id + domain bytes + newline terminator
    request = struct.pack("!H", txn_id) + domain.encode("utf-8") + b"\n"

    tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_sock.settimeout(5.0)

    try:
        tcp_sock.connect((DNS_SERVER_HOST, DNS_SERVER_PORT))
        tcp_sock.sendall(request)

        # Response is exactly 7 bytes: txn_id (2) + ip (4) + status (1)
        # TCP is a stream — must loop recv() until we have all 7 bytes.
        response = b""
        while len(response) < 7:
            chunk = tcp_sock.recv(7 - len(response))
            if not chunk:
                raise ConnectionError("DNS server closed connection before sending full response")
            response += chunk

    finally:
        tcp_sock.close()

    # Unpack: H = uint16 (txn_id), 4s = 4 raw bytes (ip), B = uint8 (status)
    rx_txn_id, ip_bytes, status = struct.unpack("!H4sB", response)

    # Validate transaction ID matches — guards against stale/mismatched responses
    if rx_txn_id != txn_id:
        raise ValueError(f"Transaction ID mismatch: sent {txn_id}, got {rx_txn_id}")

    if status == 0x01:
        raise LookupError(f"DNS NXDOMAIN: '{domain}' not found in server records")

    # Convert 4 raw bytes back to dotted IPv4 string (e.g. b'\x7f\x00\x00\x01' -> '127.0.0.1')
    ip_address = socket.inet_ntoa(ip_bytes)
    return ip_address


# Phase 2: Video Streaming 

def keepalive_sender(udp_sock, server_addr, stop_event):
    """
    Background thread: sends b"KEEPALIVE" to the server every KEEPALIVE_INTERVAL seconds.
    Stops when stop_event is set (i.e. when the main thread exits the display loop).
    This thread is a daemon so it dies automatically if the main process exits unexpectedly.
    """
    while not stop_event.is_set():
        try:
            udp_sock.sendto(b"KEEPALIVE", server_addr)
        except Exception:
            pass   # Socket may have been closed - exit gracefully
        time.sleep(KEEPALIVE_INTERVAL)


def receive_and_display(server_ip):
    """
    Main video streaming loop:
      1. Sends STREAM_START to the video server.
      2. Receives UDP packets containing frame fragments.
      3. Reassembles fragments into complete JPEG frames.
      4. Decodes and displays frames in an OpenCV window.
      5. Exits cleanly when user presses Q or closes the window.
    """
    server_addr = (server_ip, VIDEO_SERVER_PORT)

    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_sock.settimeout(UDP_RECV_TIMEOUT)

    # Signal the video server to start streaming
    udp_sock.sendto(b"STREAM_START", server_addr)
    print(f"Sent STREAM_START to {server_addr}")
    print("Press Q in the video window to quit.")

    # Start keep-alive thread
    stop_event = threading.Event()
    ka_thread = threading.Thread(
        target=keepalive_sender,
        args=(udp_sock, server_addr, stop_event),
        daemon=True
    )
    ka_thread.start()

    # Create the window explicitly before the loop
    cv2.namedWindow("Video Stream - Press Q to quit", cv2.WINDOW_AUTOSIZE)

    frame_buffer = {}
    # Track arrival time of first fragment per frame_id (for stale cleanup)
    frame_arrival = {}

    frames_displayed = 0

    try:
        while True:
            # Receive one UDP packet
            try:
                packet, _ = udp_sock.recvfrom(65535)
            except socket.timeout:
                # No packet for UDP_RECV_TIMEOUT seconds — server may have stopped
                print("No data received. Server may have stopped. Exiting.")
                break

            # Parse 12-byte header
            if len(packet) < 12:
                continue   # Malformed packet, skip

            frame_id, total_frags, frag_index = struct.unpack("!III", packet[:12])
            payload = packet[12:]

            # Store fragment
            if frame_id not in frame_buffer:
                frame_buffer[frame_id] = {}
                frame_arrival[frame_id] = time.time()

            frame_buffer[frame_id][frag_index] = payload

            # Reassemble if all fragments arrived
            if len(frame_buffer[frame_id]) == total_frags:
                # Sort by fragment index and concatenate JPEG bytes
                jpeg_bytes = b"".join(
                    frame_buffer[frame_id][i] for i in range(total_frags)
                )
                del frame_buffer[frame_id]
                del frame_arrival[frame_id]

                # Decode JPEG bytes → numpy array → BGR frame
                arr = np.frombuffer(jpeg_bytes, dtype=np.uint8)
                frame = cv2.imdecode(arr, cv2.IMREAD_COLOR)

                if frame is not None:
                    cv2.imshow("Video Stream - Press Q to quit", frame)
                    frames_displayed += 1

            # Stale frame cleanup
            # Drop partial frames that have been waiting too long (packet loss)
            now = time.time()
            stale_ids = [
                fid for fid, t in frame_arrival.items()
                if now - t > FRAME_STALE_TIMEOUT
            ]
            for fid in stale_ids:
                del frame_buffer[fid]
                del frame_arrival[fid]

            # Check for quit key
            # waitKey(1) is mandatory — without it, the OpenCV GUI event loop
            # never runs and the window appears frozen or crashes.
            key = cv2.waitKey(1) & 0xFF
            if key == ord("q") or key == ord("Q"):
                print("Q pressed. Stopping stream.")
                break

            # Detect window close button.
            # Guard with frames_displayed > 5 to let the
            # window finish initializing before we start checking.
            if frames_displayed > 5:
                try:
                    if cv2.getWindowProperty("Video Stream - Press Q to quit", cv2.WND_PROP_AUTOSIZE) < 0:
                        print("Window closed. Stopping stream.")
                        break
                except cv2.error:
                    break

    except KeyboardInterrupt:
        print("\nInterrupted.")

    finally:
        # Signal keep-alive thread to stop, then tell server we're done
        stop_event.set()
        try:
            udp_sock.sendto(b"STREAM_STOP", server_addr)
            print("Sent STREAM_STOP to server.")
        except Exception:
            pass
        udp_sock.close()
        cv2.destroyAllWindows()
        print(f"Stream ended. Displayed {frames_displayed} frame(s).")


# Entry Point 

def main():
    # Phase 1: DNS Resolution 
    print("=" * 50)
    print("Phase 1: DNS Resolution")
    print("=" * 50)

    try:
        resolved_ip = resolve_domain(DOMAIN_TO_RESOLVE)
    except LookupError as e:
        print(f"DNS Error: {e}")
        return
    except ConnectionRefusedError:
        print(f"Could not connect to DNS server at {DNS_SERVER_HOST}:{DNS_SERVER_PORT}")
        print("Is dns_server.py running?")
        return
    except Exception as e:
        print(f"Unexpected DNS error: {e}")
        return

    # Required output format per assignment spec
    print(f"\nDomain: {DOMAIN_TO_RESOLVE}")
    print(f"IP Address: {resolved_ip}\n")

    # Phase 2: Video Streaming 
    print("=" * 50)
    print("Phase 2: Video Streaming")
    print("=" * 50)

    receive_and_display(resolved_ip)


if __name__ == "__main__":
    main()