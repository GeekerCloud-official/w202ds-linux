// SPDX-License-Identifier: MIT
// Exercise the production parser and channel locks without talking to a modem.
static const char *fixture_lock_dir;
#define W202_AT_LOCK_DIR fixture_lock_dir
#define main modem_at_main
#include "../at/w202-modem-at.c"
#undef main

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "parse")) {
        char buffer[8192];
        size_t count = fread(buffer, 1, sizeof(buffer) - 1, stdin);
        buffer[count] = 0;
        return context_info(buffer);
    }
    if (argc == 4 && !strcmp(argv[1], "lock")) {
        fixture_lock_dir = argv[2];
        int fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (fd < 0) return 1;
        int lock = lock_channel(fd);
        if (lock < 0) return lock == -2 ? 75 : 1;
        puts("locked");
        fflush(stdout);
        if (!strcmp(argv[3], "hold")) (void)getchar();
        close(lock);
        close(fd);
        return 0;
    }
    return 2;
}
