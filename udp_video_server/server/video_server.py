# Video Streaming Server — UDP-based real-time webcam streamer

# Waits for a UDP packet containing b"STREAM_START" from a client.
# Streams JPEG-encoded webcam frames to that client address.
# Each frame is fragmented into chunks with a 12-byte header per packet:
#       [ frame_id (4B) ][ total_frags (4B) ][ frag_index (4B) ][ jpeg_payload ]
# Expects b"KEEPALIVE" packets from client every ~100ms.
# Stops streaming if no packet received for 200ms (keep-alive timeout).
# Stops cleanly on b"STREAM_STOP" from client.

import socket
import struct
import time
import cv2


# Maximum JPEG bytes per UDP packet.
# UDP theoretical max payload is 65507 bytes, but 60000 is a safe practical limit
# to avoid IP fragmentation across most network paths.
MAX_PAYLOAD = 60000

# If no packet from client within this many seconds, stop streaming.
KEEPALIVE_TIMEOUT = 0.200   # 200 ms

# Target frame rate
TARGET_FPS = 30
FRAME_INTERVAL = 1.0 / TARGET_FPS


def fragment_and_send(sock, jpeg_bytes, frame_id, client_addr):
    """Split jpeg_bytes into MAX_PAYLOAD chunks and send each with a header."""
    chunks = [jpeg_bytes[i:i + MAX_PAYLOAD] for i in range(0, len(jpeg_bytes), MAX_PAYLOAD)]
    total = len(chunks)
    for idx, chunk in enumerate(chunks):
        # Header: frame_id (uint32) | total_frags (uint32) | frag_index (uint32)
        header = struct.pack("!III", frame_id, total, idx)
        sock.sendto(header + chunk, client_addr)


def start_server(host="0.0.0.0", port=6000):
    # Open webcam (index 0 = default camera)
    cap = cv2.VideoCapture(0)
    if not cap.isOpened():
        print("ERROR: Cannot open webcam. Check that no other app is using it.")
        return

    print("Webcam opened successfully.")

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((host, port))
    # Non-blocking recvfrom: returns immediately if no packet waiting.
    # This lets keep-alive polling and frame sending coexist in one thread.
    sock.settimeout(0.05)

    print(f"Waiting for client on UDP {host}:{port} ...")

    # Phase 1: Block until STREAM_START received (use longer timeout here)
    sock.settimeout(60.0)
    client_addr = None
    while True:
        try:
            data, addr = sock.recvfrom(1024)
            if data == b"STREAM_START":
                client_addr = addr
                print(f"Stream requested by {client_addr}")
                break
        except socket.timeout:
            print("Still waiting for client STREAM_START ...")

    # Switch to short timeout for the streaming loop
    sock.settimeout(0.05)

    frame_id = 0
    last_keepalive = time.time()

    print("Streaming started.")

    try:
        while True:
            # Keep-alive check 
            try:
                data, addr = sock.recvfrom(1024)
                if data == b"STREAM_STOP":
                    print("Received STREAM_STOP. Stopping cleanly.")
                    break
                # Any packet from client (KEEPALIVE or other) resets the timer
                if addr == client_addr:
                    last_keepalive = time.time()
            except socket.timeout:
                pass   # No packet right now - that's fine, check timeout below

            # If client hasn't sent a keepalive in 200ms, assume it's gone
            if time.time() - last_keepalive > KEEPALIVE_TIMEOUT:
                print("Keep-alive timeout (200ms). Client likely closed. Stopping.")
                break

            #  Capture frame 
            ret, frame = cap.read()
            if not ret:
                print("WARNING: Failed to read frame from webcam. Retrying...")
                time.sleep(0.05)
                continue

            # Standardize resolution to keep frame sizes predictable
            frame = cv2.resize(frame, (640, 480))

            # Encode to JPEG with quality=70 
            encode_params = [cv2.IMWRITE_JPEG_QUALITY, 70]
            ret, buffer = cv2.imencode(".jpg", frame, encode_params)
            if not ret:
                print("WARNING: JPEG encoding failed. Skipping frame.")
                continue

            jpeg_bytes = buffer.tobytes()

            #  Fragment and send 
            fragment_and_send(sock, jpeg_bytes, frame_id, client_addr)
            frame_id += 1

            # Throttle to TARGET_FPS
            time.sleep(FRAME_INTERVAL)

    except KeyboardInterrupt:
        print("\nInterrupted. Shutting down...")
    finally:
        cap.release()
        sock.close()
        print("Server stopped.")


if __name__ == "__main__":
    start_server()