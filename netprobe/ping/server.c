/*
 * Usage: ./server <port>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <signal.h>

#define MAX_PACKET_SIZE 65507

volatile int running = 1;

void handle_sigint(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port number\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGINT, handle_sigint);

    /* Create UDP socket */
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Allow address reuse */
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Wake up every 500ms so SIGINT is handled promptly */
    struct timeval tv = { 0, 500000 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Bind */
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(port);

    if (bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("UDP Echo Server listening on port %d\n", port);
    printf("Press Ctrl+C to stop.\n\n");

    char *buffer = malloc(MAX_PACKET_SIZE);
    if (!buffer) {
        perror("malloc");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (running) {
        ssize_t n = recvfrom(sockfd, buffer, MAX_PACKET_SIZE, 0,
                             (struct sockaddr *)&client_addr, &client_len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue; /* timeout, recheck running */
            if (running) perror("recvfrom");
            break;
        }

        /* Echo the packet back unchanged */
        ssize_t sent = sendto(sockfd, buffer, n, 0,
                              (struct sockaddr *)&client_addr, client_len);
        if (sent < 0) {
            perror("sendto");
        }
    }

    printf("\nServer shutting down.\n");
    free(buffer);
    close(sockfd);
    return 0;
}