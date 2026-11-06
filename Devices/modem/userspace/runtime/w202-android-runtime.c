// SPDX-License-Identifier: MIT
// Minimal Android logging and property transport in a private Bionic chroot.
// Never sends a modem ioctl. Optional ctl support selects a fixed Linux unit.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define AREA_SIZE (128 * 1024)
#define HEADER_SIZE 128
#define VALUE_MAX 92
#define SETPROP2 0x20001u

static int make_socket(const char *path, int type, uid_t uid) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    // Refuse an existing socket; its owner must stop/clean up explicitly.
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
        chown(path, uid, (gid_t)-1) || chmod(path, 0600)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int logger(const char *path, uid_t uid) {
    int fd = make_socket(path, SOCK_DGRAM, uid);
    if (fd < 0) { perror("log socket"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (;;) {
        unsigned char buf[16384];
        ssize_t count = recv(fd, buf, sizeof(buf) - 1, 0);
        if (count < 0) { if (errno == EINTR) continue; perror("recv log"); return 1; }
        if (count < 13) continue;
        // Android logd wire header: id:u8, tid:u16, sec:u32, nsec:u32.
        uint16_t tid;
        memcpy(&tid, buf + 1, sizeof(tid));
        buf[count] = 0;
        unsigned char *tag = buf + 12;
        unsigned char *end = memchr(tag, 0, (size_t)count - 12);
        if (!end) continue;
        printf("id=%u tid=%u priority=%u %s: %s\n", buf[0], tid, buf[11], tag, end + 1);
    }
}

static int read_all(int fd, void *buffer, size_t size) {
    unsigned char *next = buffer;
    while (size) {
        ssize_t count = read(fd, next, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        next += count;
        size -= (size_t)count;
    }
    return 0;
}

static uint32_t load32(unsigned char *p) {
    return atomic_load_explicit((_Atomic uint32_t *)p, memory_order_acquire);
}

static void store32(unsigned char *p, uint32_t value) {
    atomic_store_explicit((_Atomic uint32_t *)p, value, memory_order_release);
}

static unsigned char *find_property(unsigned char *area, const char *name) {
    unsigned char *data = area + HEADER_SIZE;
    uint32_t used = load32(area), pos = 0;
    if (used < 112 || used > AREA_SIZE - HEADER_SIZE) return NULL;
    const char *token = name;
    for (unsigned int depth = 0; depth < 128; depth++) {
        const char *dot = strchr(token, '.');
        size_t len = dot ? (size_t)(dot - token) : strlen(token);
        if (pos > used - 20 || pos % 4) return NULL;
        pos = load32(data + pos + 16);
        for (unsigned int step = 0; ; step++) {
            if (!pos || pos > used - 20 || pos % 4 || step > 2048) return NULL;
            uint32_t namelen = load32(data + pos);
            if (namelen >= used - pos - 20) return NULL;
            int cmp = len < namelen ? -1 : len > namelen ? 1 : memcmp(token, data + pos + 20, len);
            if (!cmp) break;
            pos = load32(data + pos + (cmp < 0 ? 8 : 12));
        }
        if (!dot) {
            uint32_t prop = load32(data + pos + 4);
            if (!prop || prop % 4 || used < 97 || prop > used - 97) return NULL;
            if (!memchr(data + prop + 96, 0, used - prop - 96) ||
                strcmp((char *)data + prop + 96, name)) return NULL;
            return data + prop;
        }
        token = dot + 1;
    }
    return NULL;
}

static uint32_t control_cp_diskserver(const char *key, const char *value, int enabled) {
    if (!enabled || strcmp(value, "vendor.cp_diskserver")) return 0xb;
    const char *action;
    if (!strcmp(key, "ctl.start")) action = "start";
    else if (!strcmp(key, "ctl.stop")) action = "stop";
    else return 0xb;
    pid_t child = fork();
    if (child < 0) return 0x20;
    if (!child) {
        execl("/usr/bin/systemctl", "systemctl", "--no-ask-password", action,
              enabled == 2 ? "w202-cp-diskserver.service" : "w202-cp-diskserver-research.service",
              (char *)NULL);
        _exit(127);
    }
    int status;
    pid_t done;
    do { done = waitpid(child, &status, 0); } while (done < 0 && errno == EINTR);
    return done == child && WIFEXITED(status) && !WEXITSTATUS(status) ? 0 : 0x20;
}

static uint32_t update_property(unsigned char *area, const char *key, const char *value,
                                int cp_diskserver_enabled) {
    if (!strncmp(key, "ctl.", 4)) return control_cp_diskserver(key, value, cp_diskserver_enabled);
    if (!strncmp(key, "ro.", 3)) return 0xb;
    unsigned char *prop = find_property(area, key);
    if (!prop) return 0x24; // Not provisioned; never report a discarded write as success.
    uint32_t old = load32(prop);
    if ((old & 1) || (old >> 24) >= VALUE_MAX) return 0x24;
    size_t len = strlen(value);
    memcpy(area + HEADER_SIZE + 20, prop + 4, (old >> 24) + 1);
    atomic_thread_fence(memory_order_release);
    store32(prop, old | 1);
    // Ensure dirty becomes visible before the value starts changing.
    atomic_thread_fence(memory_order_seq_cst);
    memcpy(prop + 4, value, len + 1);
    store32(prop, (uint32_t)(len << 24) | ((old + 2) & 0xffffff));
    syscall(SYS_futex, prop, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    atomic_fetch_add_explicit((_Atomic uint32_t *)(area + 4), 1, memory_order_release);
    syscall(SYS_futex, area + 4, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
    return 0;
}

static int properties(const char *path, const char *filename, uid_t uid, int cp_diskserver_enabled) {
    int file = open(filename, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat info;
    if (file < 0 || fstat(file, &info) || !S_ISREG(info.st_mode) || info.st_size != AREA_SIZE) {
        perror("property area"); return 1;
    }
    unsigned char *area = mmap(NULL, AREA_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
    if (area == MAP_FAILED) { perror("mmap properties"); return 1; }
    close(file);
    if (load32(area + 8) != 0x504f5250 || load32(area + 12) != 0xfc6ed0ab) {
        fprintf(stderr, "Unsupported Bionic property area\n"); return 1;
    }
    int fd = make_socket(path, SOCK_STREAM, uid);
    if (fd < 0 || listen(fd, 8)) { perror("property socket"); return 1; }
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (;;) {
        int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
        if (client < 0) { if (errno == EINTR) continue; perror("accept property"); return 1; }
        struct ucred peer;
        socklen_t peer_len = sizeof(peer);
        struct timeval timeout = {.tv_sec = 3};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        uint32_t cmd, key_len, value_len;
        char key[256], value[VALUE_MAX];
        if (!getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) &&
            (peer.uid == uid || peer.uid == 0) && !read_all(client, &cmd, 4) && cmd == SETPROP2 &&
            !read_all(client, &key_len, 4) && key_len && key_len < sizeof(key) &&
            !read_all(client, key, key_len) && !memchr(key, 0, key_len) &&
            !read_all(client, &value_len, 4) && value_len < sizeof(value) &&
            !read_all(client, value, value_len) && !memchr(value, 0, value_len)) {
            key[key_len] = value[value_len] = 0;
            uint32_t status = update_property(area, key, value, cp_diskserver_enabled);
            printf("property uid=%u key=%s status=%u\n", peer.uid, key, status);
            if (send(client, &status, 4, MSG_NOSIGNAL) != 4) perror("property reply");
        }
        close(client);
    }
}

int main(int argc, char **argv) {
    char *end;
    if (argc < 4) goto usage;
    unsigned long uid = strtoul(argv[3], &end, 10);
    if (!*argv[3] || *end || uid > UINT_MAX) goto usage;
    signal(SIGPIPE, SIG_IGN);
    if (!strcmp(argv[1], "log") && argc == 4) return logger(argv[2], (uid_t)uid);
    if (!strcmp(argv[1], "properties") && (argc == 5 ||
        (argc == 6 && (!strcmp(argv[5], "--cp-diskserver") ||
                      !strcmp(argv[5], "--cp-diskserver-service"))))) {
        int control = argc == 5 ? 0 : !strcmp(argv[5], "--cp-diskserver-service") ? 2 : 1;
        return properties(argv[2], argv[4], (uid_t)uid, control);
    }
usage:
    fprintf(stderr, "Usage: %s log SOCKET UID | properties SOCKET UID AREA\n"
        "  [--cp-diskserver | --cp-diskserver-service]\n", argv[0]);
    return 2;
}
