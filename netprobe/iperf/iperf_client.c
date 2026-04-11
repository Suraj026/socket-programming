/*
 * Usage: ./iperf_client <server_ip> <port> <duration_sec> <packet_size> [interval_us]
 *   duration_sec : how long to run (seconds)
 *   packet_size  : bytes per packet (>= 12)
 *   interval_us  : microseconds between sends (default: 1000 = 1ms)
 *
 * Architecture:
 *   - Sender thread   : floods UDP packets to server
 *   - Receiver thread : collects echoes, updates per-second stats
 *   - Main thread     : prints stats every 1 second, writes CSV for plotting
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <math.h>

#define MIN_PACKET_SIZE 12

/* global state */

typedef struct {
    uint64_t bytes_recv;
    uint64_t pkts_recv;
    double   rtt_sum;
    uint64_t pkts_sent;
} WindowStats;

static WindowStats  g_window;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int g_done = 0;

static int          g_sockfd;
static int          g_pkt_size;
static struct sockaddr_in g_server_addr;

static uint64_t now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void pack_header(char *buf, uint32_t seq, uint64_t ts_us) {
    uint32_t seq_n = htonl(seq);
    memcpy(buf,     &seq_n, 4);
    memcpy(buf + 4, &ts_us, 8);
}

static void unpack_header(const char *buf, uint32_t *seq, uint64_t *ts_us) {
    uint32_t seq_n;
    memcpy(&seq_n,  buf,     4);
    memcpy(ts_us,   buf + 4, 8);
    *seq = ntohl(seq_n);
}

/* sender thread */

typedef struct {
    int interval_us;
} SenderArgs;

static void *sender_thread(void *arg) {
    SenderArgs *sa = (SenderArgs *)arg;
    char *buf = malloc(g_pkt_size);
    if (!buf) return NULL;
    memset(buf, 0xAB, g_pkt_size);

    uint32_t seq = 0;
    while (!g_done) {
        uint64_t ts = now_us();
        pack_header(buf, seq++, ts);

        sendto(g_sockfd, buf, g_pkt_size, 0,
               (struct sockaddr *)&g_server_addr, sizeof(g_server_addr));

        pthread_mutex_lock(&g_lock);
        g_window.pkts_sent++;
        pthread_mutex_unlock(&g_lock);

        if (sa->interval_us > 0) usleep((useconds_t)sa->interval_us);
    }
    free(buf);
    return NULL;
}

/* receiver thread */

static void *receiver_thread(void *arg) {
    (void)arg;
    char *buf = malloc(g_pkt_size + 64);
    if (!buf) return NULL;

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    while (!g_done) {
        ssize_t r = recvfrom(g_sockfd, buf, g_pkt_size + 64, 0,
                             (struct sockaddr *)&from, &from_len);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!g_done) perror("recvfrom");
            break;
        }
        uint64_t t_recv = now_us();

        uint32_t seq; uint64_t ts_send;
        if (r >= MIN_PACKET_SIZE) {
            unpack_header(buf, &seq, &ts_send);
            double rtt_ms = (t_recv - ts_send) / 1000.0;

            pthread_mutex_lock(&g_lock);
            g_window.bytes_recv += (uint64_t)r;
            g_window.pkts_recv++;
            g_window.rtt_sum += rtt_ms;
            pthread_mutex_unlock(&g_lock);
        }
    }
    free(buf);
    return NULL;
}


int main(int argc, char *argv[]) {
    if (argc < 5 || argc > 6) {
        fprintf(stderr,
            "Usage: %s <server_ip> <port> <duration_sec> <packet_size> [interval_us]\n",
            argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip   = argv[1];
    int         port        = atoi(argv[2]);
    int         duration    = atoi(argv[3]);
    g_pkt_size              = atoi(argv[4]);
    int         interval_us = (argc == 6) ? atoi(argv[5]) : 1000;

    if (g_pkt_size < MIN_PACKET_SIZE) {
        fprintf(stderr, "packet_size must be >= %d\n", MIN_PACKET_SIZE);
        exit(EXIT_FAILURE);
    }

    g_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    /* Short timeout so receiver thread can check g_done */
    struct timeval tv = { 0, 100000 };  /* 100 ms */
    setsockopt(g_sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&g_server_addr, 0, sizeof(g_server_addr));
    g_server_addr.sin_family = AF_INET;
    g_server_addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, server_ip, &g_server_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid server IP\n"); exit(EXIT_FAILURE);
    }

    /* CSV output file */
    FILE *csv = fopen("throughput_stats.csv", "w");
    if (!csv) { perror("fopen csv"); exit(EXIT_FAILURE); }
    fprintf(csv, "time_s,throughput_mbps,avg_delay_ms,pkts_sent,pkts_recv\n");

    printf("iPerf-like UDP Test: %s:%d | %d sec | %d bytes/pkt | %d us interval\n\n",
           server_ip, port, duration, g_pkt_size, interval_us);
    printf("%-8s %-18s %-16s %-12s %-12s\n",
           "Time(s)", "Throughput(Mbps)", "AvgDelay(ms)", "PktsSent", "PktsRecv");
    printf("%-8s %-18s %-16s %-12s %-12s\n",
           "------", "----------------", "------------", "--------", "--------");

    /* Start threads */
    SenderArgs sa = { interval_us };
    pthread_t tid_send, tid_recv;
    pthread_create(&tid_recv, NULL, receiver_thread, NULL);
    pthread_create(&tid_send, NULL, sender_thread,   &sa);

    for (int t = 1; t <= duration; t++) {
        sleep(1);

        /* Snapshot and reset window */
        pthread_mutex_lock(&g_lock);
        WindowStats snap = g_window;
        memset(&g_window, 0, sizeof(g_window));
        pthread_mutex_unlock(&g_lock);

        double throughput_mbps = (snap.bytes_recv * 8.0) / 1e6;
        double avg_delay_ms    = snap.pkts_recv > 0
                                 ? snap.rtt_sum / snap.pkts_recv : 0.0;

        printf("%-8d %-18.3f %-16.3f %-12lu %-12lu\n",
               t, throughput_mbps, avg_delay_ms,
               (unsigned long)snap.pkts_sent, (unsigned long)snap.pkts_recv);

        fprintf(csv, "%d,%.3f,%.3f,%lu,%lu\n",
                t, throughput_mbps, avg_delay_ms,
                (unsigned long)snap.pkts_sent, (unsigned long)snap.pkts_recv);
        fflush(csv);
    }

    g_done = 1;
    pthread_join(tid_send, NULL);
    pthread_join(tid_recv, NULL);

    fclose(csv);
    close(g_sockfd);

    printf("\nStats written to throughput_stats.csv\n");
    return 0;
}