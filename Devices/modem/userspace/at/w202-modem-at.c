// SPDX-License-Identifier: MIT
// AT diagnostics for Unisoc SIPC; changing functionality requires an explicit option.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#ifndef W202_AT_LOCK_DIR
#define W202_AT_LOCK_DIR "/run/w202-modem"
#endif

static long long now_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) { perror("clock_gettime"); exit(1); }
    return (long long)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, long long deadline) {
    for (;;) {
        long long left = deadline - now_ms();
        if (left <= 0) { errno = ETIMEDOUT; return -1; }
        struct pollfd pollfd = {.fd = fd, .events = events};
        int rc = poll(&pollfd, 1, (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { if (!rc) errno = ETIMEDOUT; return -1; }
        if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; return -1; }
        if (pollfd.revents & events) return 0;
    }
}

// Lock the underlying character device, so aliases of one channel cannot race.
// Never drain pending bytes before acquiring this lock.
static int lock_channel(int fd) {
    struct stat channel, directory, lock;
    if (fstat(fd, &channel) || !S_ISCHR(channel.st_mode)) {
        fprintf(stderr, "AT channel must be a character device\n");
        return -1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "Run AT diagnostics as root to serialize the channel\n");
        return -1;
    }
    if (mkdir(W202_AT_LOCK_DIR, 0755) && errno != EEXIST) {
        perror("create AT lock directory"); return -1;
    }
    int dir = open(W202_AT_LOCK_DIR, O_DIRECTORY | O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) { perror("open AT lock directory"); return -1; }
    if (fstat(dir, &directory) || directory.st_uid != 0 || (directory.st_mode & 0022)) {
        fprintf(stderr, "AT lock directory must be root-owned and not writable by others\n");
        close(dir); return -1;
    }
    char name[64];
    snprintf(name, sizeof(name), "channel-%u-%u.lock", major(channel.st_rdev), minor(channel.st_rdev));
    int descriptor = openat(dir, name, O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    close(dir);
    if (descriptor < 0) { perror("open AT lock"); return -1; }
    if (fstat(descriptor, &lock) || !S_ISREG(lock.st_mode) || lock.st_uid != 0 ||
        lock.st_nlink != 1 || (lock.st_mode & 0022)) {
        fprintf(stderr, "AT lock must be a private root-owned regular file\n");
        close(descriptor); return -1;
    }
    if (flock(descriptor, LOCK_EX | LOCK_NB)) {
        int busy = errno == EWOULDBLOCK;
        if (busy) fprintf(stderr, "AT channel busy; retry after the current transaction\n");
        else perror("lock AT channel");
        close(descriptor); return busy ? -2 : -1;
    }
    return descriptor;
}

static int ipv4(const char *value, char output[INET_ADDRSTRLEN]) {
    struct in_addr address;
    if (inet_pton(AF_INET, value, &address) != 1) return -1;
    unsigned first = ntohl(address.s_addr) >> 24;
    if (!first || first == 127 || first >= 224) return -1;
    return inet_ntop(AF_INET, &address, output, INET_ADDRSTRLEN) ? 0 : -1;
}

// The local modem returns IPv4 followed by its four netmask octets in field 4.
// Keep the interface /32: M-ETHER is an L3 link, not the operator's LAN.
static int context_info(char *response) {
    for (char *line = response; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "+CGCONTRDP:", 11)) {
            const char *cursor = line + 11;
            char fields[8][160] = {{0}};
            for (size_t field = 0; field < 8; field++) {
                while (*cursor == ' ') cursor++;
                int quoted = *cursor == '"';
                if (quoted) cursor++;
                size_t used = 0;
                while (*cursor && *cursor != '\r' &&
                    (quoted ? *cursor != '"' : *cursor != ',')) {
                    if (used + 1 == sizeof(fields[field])) goto invalid;
                    fields[field][used++] = *cursor++;
                }
                if (quoted && *cursor++ != '"') goto invalid;
                while (*cursor == ' ') cursor++;
                if (field < 7) {
                    if (*cursor != ',') goto invalid;
                    cursor++;
                } else if (*cursor && *cursor != ',' && *cursor != '\r') goto invalid;
            }
            if (strcmp(fields[0], "1")) { line = next; continue; }
            char *local = fields[3], *mask = local;
            for (int dot = 0; dot < 4; dot++) {
                mask = strchr(mask, '.');
                if (!mask) break;
                if (dot == 3) *mask++ = 0;
                else mask++;
            }
            char address[INET_ADDRSTRLEN], dns1[INET_ADDRSTRLEN] = "", dns2[INET_ADDRSTRLEN] = "";
            if (ipv4(local, address)) goto invalid;
            if (mask) {
                struct in_addr netmask;
                if (inet_pton(AF_INET, mask, &netmask) != 1) goto invalid;
                unsigned inverse = ~ntohl(netmask.s_addr);
                if (inverse & (inverse + 1U)) goto invalid;
            }
            if (*fields[5] && strcmp(fields[5], "0.0.0.0") && ipv4(fields[5], dns1)) goto invalid;
            if (*fields[6] && strcmp(fields[6], "0.0.0.0") && ipv4(fields[6], dns2)) goto invalid;
            if (!*dns1 && !*dns2) goto invalid;
            printf("IPV4=%s\nDNS1=%s\nDNS2=%s\n", address, dns1, dns2);
            return 0;
        }
        line = next;
    }
    fprintf(stderr, "No active IPv4 CID1 context in modem response\n");
    return 4;
invalid:
    fprintf(stderr, "Invalid IPv4 CID1 address/DNS response\n");
    return 4;
}

int main(int argc, char **argv) {
    static const char *queries[] = {"AT", "AT+CGMI", "AT+CGMM", "AT+CFUN?", "AT+CPIN?",
        "AT+CREG?", "AT+CGREG?", "AT+CEREG?", "AT+C5GREG?", "AT+CSQ", "AT+CESQ",
        "AT+CGATT?", "AT+CGACT?", "AT+CGDCONT?", "AT+CGPADDR=1", "AT+CGCONTRDP=1"};
    const char *command = argc >= 3 ? argv[2] : "AT";
    char custom[160];
    int action = 0, connect = 0, info = 0, timeout_ms = 5000;
    if (!strcmp(command, "--context-info")) { command = "AT+CGCONTRDP=1"; info = 1; }
    if (!strcmp(command, "--radio-on")) { command = "AT+CFUN=1"; action = 1; }
    if (!strcmp(command, "--radio-off")) { command = "AT+CFUN=0"; action = 1; }
    if (!strcmp(command, "--activate-context")) { command = "AT+CGACT=1,1"; action = 1; timeout_ms = 45000; }
    if (!strcmp(command, "--deactivate-context")) { command = "AT+CGACT=0,1"; action = 1; timeout_ms = 45000; }
    if (!strcmp(command, "--connect-context")) {
        command = "AT+CGDATA=\"M-ETHER\",1"; action = connect = 1; timeout_ms = 45000;
    }
    if (argc == 4 && !strcmp(command, "--define-context")) {
        size_t size = strlen(argv[3]);
        if (!size || size > 100) goto usage;
        for (size_t i = 0; i < size; i++)
            if (!isalnum((unsigned char)argv[3][i]) && argv[3][i] != '.' && argv[3][i] != '-') goto usage;
        // The original local RIL uses these extended parameters for IPv4 contexts.
        snprintf(custom, sizeof(custom), "AT+CGDCONT=1,\"IP\",\"%s\",\"\",0,0,0,0,1", argv[3]);
        command = custom; action = 1;
    } else if (argc != 2 && argc != 3) goto usage;
    const char *prefix = "/dev/stty_nr";
    if (strncmp(argv[1], prefix, strlen(prefix))) goto usage;
    char *end;
    const char *number = argv[1] + strlen(prefix);
    long channel = strtol(number, &end, 10);
    if (!*number || *end || channel < 0 || channel > 31 ||
        (*number == '0' && number[1]) || *number < '0' || *number > '9') goto usage;
    int allowed = action;
    for (size_t i = 0; i < sizeof(queries) / sizeof(queries[0]); i++)
        if (!strcmp(command, queries[i])) allowed = 1;
    if (!allowed) goto usage;
    int fd = open(argv[1], O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
    if (fd < 0) { perror("open AT channel"); return 1; }
    int channel_lock = lock_channel(fd);
    if (channel_lock < 0) { close(fd); return channel_lock == -2 ? 75 : 1; }
    char buffer[8192];
    size_t drained = 0;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == ENODATA)) break;
        if (count < 0) { perror("drain AT channel"); close(fd); return 1; }
        if (!count) break;
        drained += (size_t)count;
        if (drained >= 65536) { fprintf(stderr, "AT channel has continuous pending data\n"); close(fd); return 1; }
    }
    char request[164];
    int length = snprintf(request, sizeof(request), "%s\r", command);
    long long deadline = now_ms() + timeout_ms;
    size_t sent = 0;
    while (sent < (size_t)length) {
        if (wait_fd(fd, POLLOUT, deadline)) { perror("AT write wait"); close(fd); return 1; }
        ssize_t count = write(fd, request + sent, (size_t)length - sent);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count <= 0) { perror("AT write"); close(fd); return 1; }
        sent += (size_t)count;
    }
    size_t used = 0;
    buffer[0] = 0;
    while (used < sizeof(buffer) - 1) {
        if (wait_fd(fd, POLLIN, deadline)) { perror("AT response wait"); break; }
        ssize_t count = read(fd, buffer + used, sizeof(buffer) - 1 - used);
        if (count < 0 && (errno == EAGAIN || errno == EINTR || errno == ENODATA)) continue;
        if (count <= 0) { perror("AT read"); break; }
        used += (size_t)count;
        buffer[used] = 0;
        int ok = connect ? (strstr(buffer, "\nCONNECT") != NULL || !strncmp(buffer, "CONNECT", 7)) :
            (strstr(buffer, "\nOK\r\n") != NULL || !strcmp(buffer, "OK\r\n"));
        int error = strstr(buffer, "\nERROR\r\n") != NULL ||
            strstr(buffer, "+CME ERROR:") != NULL || strstr(buffer, "+CMS ERROR:") != NULL;
        if (ok || error) {
            if (info && !error) return context_info(buffer);
            fwrite(buffer, 1, used, stdout);
            close(fd);
            return error ? 3 : 0;
        }
    }
    fwrite(buffer, 1, used, stdout);
    close(fd);
    return 1;
usage:
    fprintf(stderr, "Usage: %s /dev/stty_nr0..31 [AT query | --radio-on | --radio-off |\n"
        "  --define-context APN | --activate-context | --connect-context | --deactivate-context | --context-info]\n"
        "Data controls address CID 1 only. M-ETHER connect/activation wait up to 45 seconds.\n", argv[0]);
    return 2;
}
