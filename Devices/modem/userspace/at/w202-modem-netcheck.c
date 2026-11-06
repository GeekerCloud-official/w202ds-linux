// SPDX-License-Identifier: MIT
// A source-bound UDP DNS probe, so a Wi-Fi route cannot satisfy a cellular test.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv) {
    struct sockaddr_in source = {.sin_family = AF_INET};
    struct sockaddr_in server = {.sin_family = AF_INET, .sin_port = htons(53)};
    if (argc != 3 || inet_pton(AF_INET, argv[1], &source.sin_addr) != 1 ||
        inet_pton(AF_INET, argv[2], &server.sin_addr) != 1) {
        fprintf(stderr, "usage: %s SOURCE_IPV4 DNS_SERVER_IPV4\n", argv[0]);
        return 2;
    }
    unsigned char query[] = {
        0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
        7, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        3, 'c', 'o', 'm', 0, 0, 1, 0, 1
    };
    ssize_t count;
    do count = getrandom(query, 2, 0); while (count < 0 && errno == EINTR);
    if (count != 2) { perror("DNS transaction ID"); return 1; }
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return 1; }
    if (bind(fd, (struct sockaddr *)&source, sizeof(source)) ||
        connect(fd, (struct sockaddr *)&server, sizeof(server))) {
        perror("bind/connect DNS"); close(fd); return 1;
    }
    socklen_t size = sizeof(source);
    if (getsockname(fd, (struct sockaddr *)&source, &size)) {
        perror("DNS source address"); close(fd); return 1;
    }
    printf("DNS source=%s:%u server=%s:53 query=example.com/A\n",
           argv[1], ntohs(source.sin_port), argv[2]);
    do count = send(fd, query, sizeof(query), 0); while (count < 0 && errno == EINTR);
    if (count != (ssize_t)sizeof(query)) { perror("send DNS"); close(fd); return 1; }
    struct pollfd event = {.fd = fd, .events = POLLIN};
    int ready;
    do ready = poll(&event, 1, 5000); while (ready < 0 && errno == EINTR);
    if (ready <= 0) {
        if (!ready) errno = ETIMEDOUT;
        perror("DNS response"); close(fd); return 1;
    }
    unsigned char response[512];
    do count = recv(fd, response, sizeof(response), MSG_DONTWAIT); while (count < 0 && errno == EINTR);
    close(fd);
    if (count < 0) { perror("receive DNS"); return 1; }
    if (count < 12 || memcmp(query, response, 2) || !(response[2] & 0x80)) {
        fprintf(stderr, "invalid DNS response\n"); return 1;
    }
    unsigned int rcode = response[3] & 15;
    unsigned int answers = ((unsigned int)response[6] << 8) | response[7];
    printf("DNS_RESPONSE_BYTES=%zd RCODE=%u ANSWERS=%u TRUNCATED=%u\n",
           count, rcode, answers, !!(response[2] & 2));
    return rcode ? 1 : 0;
}
