# client to server packet format:
# [ txn_id (2 bytes) ][ domain (N bytes) ][ '\n' (1 byte) ]
# server to client packet format:
# [ txn_id (2 bytes) ][ ip (4 bytes) ][ status (1 byte) ]

import socket
import threading
import struct

def load_dns_records():
    dns_map = {}
    try:
        with open("records/dns_records.txt", "r") as f:
            for mapping in f.readlines():
                domain, ip = mapping.strip().split()
                dns_map[domain] = ip
    except FileNotFoundError:
        print("DNS records file not found")
    return dns_map

RECORDS = load_dns_records() # load DNS records into memory 

def handle_client(conn):
    try:
        buffer = b""    # create a buffer to store incoming data

        while True:
            data = conn.rev(1024)   # receive data from the client
            if not data:
                break   # if no data is received, break the loop
            buffer += data  # append received data to the buffer
            if b"\n" in buffer:
                break   # if a newline is found in the buffer, break the loop
        if b"\n" not in buffer or len(buffer) < 3:
            return  # if no newline is found or the buffer is too short, return
        
        # extract transaction ID
        txn_id = struct.unpack("!H", buffer[:2])[0]  # unpack the first 2 bytes as transaction ID
        # extract domain name
        domain_end = buffer.find(b"\n")
        domain_bytes = buffer[2:domain_end]  # extract domain name bytes
        domain = domain_bytes.decode().strip().lower()  # decode and strip the domain name

        # lookup in DNS records
        ip = RECORDS.get(domain, "")
        if ip:
            ip_bytes = socket.inet_aton(ip)  # convert IP address to bytes
            status = 1
        else:
            ip_bytes = b"\x00\x00\x00\x00"  # return
            status = 0

        # pack response
        response = struct.pack("!H4sB", txn_id, ip_bytes, status)
        # send full response
        conn.sendall(response)
        
    except Exception as e:
        print(f"Error handling client: {e}")

    finally:
        conn.close()  # close the connection when done

def start_server(host = "0.0.0.0", port = 5000):
    # create a socket object
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # avoid TIME_WAIT lock issues
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # bind the socket to the address and port
    sock.bind((host, port))
    # start listening for incoming connections
    sock.listen()
    print(f"DNS Server started on {host}:{port}")

    while True:
        try :
            # accept a connection
            conn, addr = sock.accept()
            print(f"Connection from {addr}")
            # handle connection in a new thread
            thread = threading.Thread(target=handle_client, args=(conn,), daemon=True)
            thread.start()
        except KeyboardInterrupt:
            print(f"\nShutting down DNS server...")
            break
        except Exception as e:
            print(f"Error: {e}")
    
    sock.close()

if __name__ == "__main__":
    start_server()