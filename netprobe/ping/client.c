/*
 * Packet format (minimum 12 bytes):
 *   [0..3]  : uint32_t sequence number (network byte order)
 *   [4..11] : uint64_t send timestamp in microseconds (host byte order)
 *   [12..]  : padding bytes (0xAB)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <stdint.h>
#include <errno.h>

#define MIN_PACKET_SIZE  12      /* seq(4) + timestamp(8) */
#define RECV_TIMEOUT_SEC  2      /* 2-second receive timeout */

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void pack_header(char *buf, uint32_t seq, uint64_t ts_us) {
    uint32_t seq_n = htonl(seq);
    memcpy(buf,     &seq_n,  4);
    memcpy(buf + 4, &ts_us,  8);   /* timestamp in host byte order */
}

static void unpack_header(const char *buf, uint32_t *seq, uint64_t *ts_us) {
    uint32_t seq_n;
    memcpy(&seq_n,  buf,     4);
    memcpy(ts_us,   buf + 4, 8);
    *seq = ntohl(seq_n);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr,
            "Usage: %s <server_ip> <port> <num_packets> <interval_ms> <packet_size>\n",
            argv[0]);
        fprintf(stderr, "  packet_size : minimum %d bytes\n", MIN_PACKET_SIZE);
        exit(EXIT_FAILURE);
    }

    const char *server_ip   = argv[1];
    int         port        = atoi(argv[2]);
    int         num_packets = atoi(argv[3]);
    int         interval_ms = atoi(argv[4]);
    int         pkt_size    = atoi(argv[5]);

    if (pkt_size < MIN_PACKET_SIZE) {
        fprintf(stderr, "packet_size must be >= %d\n", MIN_PACKET_SIZE);
        exit(EXIT_FAILURE);
    }
    if (num_packets <= 0 || port <= 0 || interval_ms < 0) {
        fprintf(stderr, "Invalid arguments.\n");
        exit(EXIT_FAILURE);
    }

    /* Create UDP socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    /* Set receive timeout */
    struct timeval tv = { RECV_TIMEOUT_SEC, 0 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Server address */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP: %s\n", server_ip);
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    char *send_buf = malloc(pkt_size);
    char *recv_buf = malloc(pkt_size);
    if (!send_buf || !recv_buf) { perror("malloc"); exit(EXIT_FAILURE); }

    memset(send_buf, 0xAB, pkt_size);   /* fill padding */

    /* Statistics */
    int     sent     = 0;
    int     received = 0;
    double  rtt_min  = 1e18;
    double  rtt_max  = 0.0;
    double  rtt_sum  = 0.0;

    printf("Pinging %s:%d — %d packets, %d ms interval, %d bytes\n\n",
           server_ip, port, num_packets, interval_ms, pkt_size);

    for (int seq = 0; seq < num_packets; seq++) {
        /* Build packet */
        uint64_t t_send = now_us();
        pack_header(send_buf, (uint32_t)seq, t_send);

        /* Send */
        ssize_t n = sendto(sockfd, send_buf, pkt_size, 0,
                           (struct sockaddr *)&server_addr, sizeof(server_addr));
        if (n < 0) { perror("sendto"); continue; }
        sent++;

        /* Receive */
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t r = recvfrom(sockfd, recv_buf, pkt_size, 0,
                             (struct sockaddr *)&from, &from_len);
        uint64_t t_recv = now_us();

        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                printf("seq=%d  timeout\n", seq);
            else
                perror("recvfrom");
            continue;
        }

        /* Parse echo */
        uint32_t echo_seq;
        uint64_t echo_ts;
        unpack_header(recv_buf, &echo_seq, &echo_ts);

        double rtt_ms = (t_recv - echo_ts) / 1000.0;
        received++;

        if (rtt_ms < rtt_min) rtt_min = rtt_ms;
        if (rtt_ms > rtt_max) rtt_max = rtt_ms;
        rtt_sum += rtt_ms;

        printf("seq=%-5u  rtt=%.3f ms  bytes=%zd\n", echo_seq, rtt_ms, r);

        /* Sleep for interval (minus time already spent) */
        if (interval_ms > 0) {
            uint64_t elapsed_us = now_us() - t_send;
            long sleep_us = (long)interval_ms * 1000 - (long)elapsed_us;
            if (sleep_us > 0) usleep((useconds_t)sleep_us);
        }
    }

    /* Summary */
    int lost = sent - received;
    double loss_pct = sent > 0 ? (lost * 100.0 / sent) : 0.0;

    printf("\n--- %s ping statistics ---\n", server_ip);
    printf("%d packets transmitted, %d received, %.1f%% packet loss\n",
           sent, received, loss_pct);
    if (received > 0) {
        printf("rtt min/avg/max = %.3f/%.3f/%.3f ms\n",
               rtt_min, rtt_sum / received, rtt_max);
    }

    free(send_buf);
    free(recv_buf);
    close(sockfd);
    return 0;
}