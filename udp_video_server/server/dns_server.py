# DNS Server — TCP-based custom DNS resolver
# Client -> Server packet format: [ txn_id (2 bytes, big-endian uint16) ][ domain (N bytes, UTF-8) ][ '\n' (1 byte) ]
# Server -> Client packet format: [ txn_id (2 bytes, big-endian uint16) ][ ip (4 bytes, network order) ][ status (1 byte) ]
# status: 0x00 = found (NOERROR), 0x01 = not found (NXDOMAIN)

import socket
import threading
import struct

def load_dns_records(filepath="records/dns_records.txt"):
    dns_map = {}
    try:
        with open(filepath, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = line.split()
                if len(parts) >= 2:
                    domain, ip = parts[0].lower(), parts[1]
                    dns_map[domain] = ip
        print("DNS server started...")
        print(f"Loaded {len(dns_map)} record(s) from {filepath}")
    except FileNotFoundError:
        print(f"ERROR: Records file not found at '{filepath}'")
        print("Create 'records/dns_records.txt' with entries like: video.server.com 127.0.0.1")
    return dns_map


RECORDS = load_dns_records()


def recv_exact(conn, n):
    """Read exactly n bytes from a TCP connection, looping as needed."""
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("Connection closed before all bytes received")
        buf += chunk
    return buf


def handle_client(conn, addr):
    try:
        # Read until newline terminator : TCP is a stream, recv() may return partial data
        buffer = b""
        while b"\n" not in buffer:
            chunk = conn.recv(1024)
            if not chunk:
                print(f"Connection closed by {addr} before full request")
                return
            buffer += chunk

        # Validate minimum packet size: 2 bytes txn_id + at least 1 byte domain + '\n'
        if len(buffer) < 3:
            print(f"Malformed request from {addr} (too short)")
            return

        # Parse transaction ID (first 2 bytes, big-endian)
        txn_id = struct.unpack("!H", buffer[:2])[0]

        # Parse domain name (bytes between txn_id and newline)
        newline_pos = buffer.find(b"\n")
        domain = buffer[2:newline_pos].decode("utf-8").strip().lower()

        print(f"Query from {addr} : txn_id={txn_id}, domain='{domain}'")

        # Lookup domain in records
        ip_str = RECORDS.get(domain, "")

        if ip_str:
            ip_bytes = socket.inet_aton(ip_str)   # dotted string -> 4 raw bytes
            status = 0x00                          # found
            print(f"Resolved '{domain}' → {ip_str}")
        else:
            ip_bytes = b"\x00\x00\x00\x00"
            status = 0x01                          # not found
            print(f"NXDOMAIN for '{domain}'")

        # Pack and send response: txn_id (2B) + ip (4B) + status (1B) = 7 bytes
        response = struct.pack("!H4sB", txn_id, ip_bytes, status)
        conn.sendall(response)   # send all data

    except Exception as e:
        print(f"Error handling {addr}: {e}")
    finally:
        conn.close()


def start_server(host="0.0.0.0", port=5000):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # SO_REUSEADDR prevents "Address already in use" error on quick restarts
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((host, port))
    sock.listen(5)
    print(f"Server listening on {host}:{port}")

    try:
        while True:
            conn, addr = sock.accept()
            # Each client gets its own daemon thread, server stays responsive
            t = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
            t.start()
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        sock.close()


if __name__ == "__main__":
    start_server()