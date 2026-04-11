/*
 * Usage (requires root):
 *   sudo ./traceroute <destination_ip> [max_hops] [probes_per_hop] [timeout_ms]
 *
 * Defaults: max_hops=30, probes_per_hop=3, timeout_ms=3000
 *
 * Design:
 *   send_sock : SOCK_DGRAM UDP, destination port = BASE_PORT + ttl
 *   recv_sock : SOCK_RAW, IPPROTO_ICMP
 *
 * ICMP types handled:
 *   11 (Time Exceeded)       — intermediate hop
 *    3 (Destination Unreach) — destination reached (port unreachable)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/udp.h>

#include <arpa/inet.h>

#define BASE_PORT     33434
#define PROBE_PAYLOAD 32       /* bytes of UDP payload */
#define MAX_HOPS_DEF  30
#define PROBES_DEF     3
#define TIMEOUT_MS_DEF 3000

/* timing */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* receive one ICMP packet and check relevance */
/*
 * Returns:
 *   1  — received relevant ICMP (fills hop_ip, sets *icmp_type)
 *   0  — timeout
 *  -1  — error
 */
static int recv_icmp(int raw_sock, uint32_t dest_ip,
                     int cur_ttl,
                     char *hop_ip_str,        /* out: "a.b.c.d" */
                     int  *icmp_type,         /* out: 11 or 3    */
                     int   timeout_ms)
{
    unsigned char buf[512];
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);

    /* Use select() for timeout */
    fd_set rset;
    FD_ZERO(&rset);
    FD_SET(raw_sock, &rset);
    struct timeval tv = {
        timeout_ms / 1000,
        (timeout_ms % 1000) * 1000
    };

    int sel = select(raw_sock + 1, &rset, NULL, NULL, &tv);
    if (sel < 0) { perror("select"); return -1; }
    if (sel == 0) return 0;   /* timeout */

    ssize_t n = recvfrom(raw_sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&from, &from_len);
    if (n < 0) { perror("recvfrom raw"); return -1; }

    /* Parse outer IP header */
    struct ip *ip_hdr = (struct ip *)buf;
    int ip_hlen = ip_hdr->ip_hl * 4;
    if (n < ip_hlen + 8) return -1;  /* too short */

    /* ICMP header starts after outer IP */
    struct icmp *icmp_hdr = (struct icmp *)(buf + ip_hlen);
    uint8_t type = icmp_hdr->icmp_type;
    uint8_t code = icmp_hdr->icmp_code;

    /*
     * For Time Exceeded (type 11) and Destination Unreachable (type 3),
     * the ICMP payload contains the original IP + UDP headers.
     * We verify dest IP and dest port to ensure this is our probe.
     */
    if (type == ICMP_TIMXCEED || type == ICMP_UNREACH) {
        int icmp_hdr_len = 8;
        unsigned char *inner = buf + ip_hlen + icmp_hdr_len;
        int remaining = n - ip_hlen - icmp_hdr_len;

        if (remaining < (int)(sizeof(struct ip) + sizeof(struct udphdr)))
            return -1;

        struct ip  *inner_ip  = (struct ip *)inner;
        struct udphdr *inner_udp = (struct udphdr *)(inner + inner_ip->ip_hl * 4);

        /* Verify destination IP and port match our probe */
        if (inner_ip->ip_dst.s_addr != dest_ip) return -1;
        int expected_port = BASE_PORT + cur_ttl;
        if (ntohs(inner_udp->uh_dport) != expected_port) return -1;

        inet_ntop(AF_INET, &from.sin_addr, hop_ip_str, INET_ADDRSTRLEN);
        *icmp_type = type;
        (void)code;
        return 1;
    }

    return -1;  
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage: sudo %s <dest_ip> [max_hops] [probes] [timeout_ms]\n",
            argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *dest_str   = argv[1];
    int max_hops   = (argc > 2) ? atoi(argv[2]) : MAX_HOPS_DEF;
    int n_probes   = (argc > 3) ? atoi(argv[3]) : PROBES_DEF;
    int timeout_ms = (argc > 4) ? atoi(argv[4]) : TIMEOUT_MS_DEF;

    /* Validate destination IP */
    struct in_addr dest_addr;
    if (inet_pton(AF_INET, dest_str, &dest_addr) <= 0) {
        fprintf(stderr, "Invalid destination IP: %s\n", dest_str);
        exit(EXIT_FAILURE);
    }
    uint32_t dest_ip = dest_addr.s_addr;

    /* UDP send socket */
    int send_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (send_sock < 0) { perror("UDP socket"); exit(EXIT_FAILURE); }

    /* Raw ICMP receive socket */
    int recv_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (recv_sock < 0) {
        perror("RAW socket (need root)");
        close(send_sock);
        exit(EXIT_FAILURE);
    }

    /* Probe payload */
    char payload[PROBE_PAYLOAD];
    memset(payload, 0x42, sizeof(payload));

    printf("traceroute to %s, max %d hops, %d probes per hop\n\n",
           dest_str, max_hops, n_probes);

    int reached = 0;

    for (int ttl = 1; ttl <= max_hops && !reached; ttl++) {
        /* Set TTL on the send socket */
        if (setsockopt(send_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0) {
            perror("setsockopt IP_TTL");
            break;
        }

        /* Destination port encodes TTL for matching in recv */
        int dest_port = BASE_PORT + ttl;

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family      = AF_INET;
        dest.sin_addr.s_addr = dest_ip;
        dest.sin_port        = htons(dest_port);

        printf("%2d  ", ttl);
        fflush(stdout);

        char last_hop[INET_ADDRSTRLEN] = "";

        for (int p = 0; p < n_probes; p++) {
            double t_send = now_ms();

            ssize_t s = sendto(send_sock, payload, sizeof(payload), 0,
                               (struct sockaddr *)&dest, sizeof(dest));
            if (s < 0) { perror("sendto probe"); continue; }

            char hop_ip[INET_ADDRSTRLEN];
            int  icmp_type = 0;
            int  status = recv_icmp(recv_sock, dest_ip,
                                    ttl,
                                    hop_ip, &icmp_type, timeout_ms);

            if (status == 0) {
                /* Timeout */
                printf("  *");
                fflush(stdout);
            } else if (status == 1) {
                double rtt = now_ms() - t_send;

                /* Print IP if first probe or changed */
                if (strcmp(hop_ip, last_hop) != 0) {
                    printf("  %s", hop_ip);
                    snprintf(last_hop, INET_ADDRSTRLEN, "%s", hop_ip);
                }
                printf("  %.3f ms", rtt);
                fflush(stdout);

                /* Check if we reached the destination */
                if (icmp_type == ICMP_UNREACH) {
                    if (inet_addr(hop_ip) == dest_ip) {
                        reached = 1;
                    }
                }
            }
        }

        printf("\n");

        if (reached) {
            printf("\nDestination %s reached.\n", dest_str);
        }
    }

    if (!reached) {
        printf("\nMax hops (%d) exceeded. Destination not reached.\n", max_hops);
    }

    close(send_sock);
    close(recv_sock);
    return 0;
}