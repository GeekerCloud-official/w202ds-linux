// SPDX-License-Identifier: GPL-2.0-only
/* Raw GPIO-v2 edge diagnostics for the stock W202DS Hall line. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;

static void stop(int signal)
{
    interrupted = signal;
}

static int64_t now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        return -1;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    const char *chip_path = "/dev/gpiochip4";
    struct gpiochip_info chip = {0};
    struct gpio_v2_line_info info = {.offset = 148};
    struct gpio_v2_line_request request = {0};
    struct gpio_v2_line_values values = {.mask = 1};
    struct sigaction action = {.sa_handler = stop};
    bool info_only = argc == 2 && !strcmp(argv[1], "--info");
    unsigned events = 0, missed = 0, previous_seq = 0;
    long seconds = 15;
    int chip_fd = -1, event_fd = -1, result = 1;
    int64_t deadline;

    if (!info_only && argc > 1) {
        char *end;
        errno = 0;
        seconds = strtol(argv[1], &end, 10);
        if (argc != 2 || errno || !*argv[1] || *end || seconds < 1 || seconds > 300) {
            fprintf(stderr, "usage: %s [--info|seconds(1..300)]\n", argv[0]);
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    chip_fd = open(chip_path, O_RDONLY | O_CLOEXEC);
    if (chip_fd < 0 || ioctl(chip_fd, GPIO_GET_CHIPINFO_IOCTL, &chip)) {
        perror("GPIO chip information");
        goto out;
    }
    if (strcmp(chip.label, "64170000.gpio") || chip.lines != 256) {
        fprintf(stderr, "unexpected GPIO controller; no line requested\n");
        goto out;
    }
    if (ioctl(chip_fd, GPIO_V2_GET_LINEINFO_IOCTL, &info)) {
        perror("GPIO line information");
        goto out;
    }
    printf("{\"event\":\"line-info\",\"chip\":\"64170000.gpio\",\"offset\":148,\"flags\":%" PRIu64 ",\"used\":%s}\n",
           (uint64_t)info.flags, info.flags & GPIO_V2_LINE_FLAG_USED ? "true" : "false");
    if (info_only) {
        result = 0;
        goto out;
    }
    if (info.flags & GPIO_V2_LINE_FLAG_USED) {
        fprintf(stderr, "Hall line already owned; refusing to change it\n");
        result = 75;
        goto out;
    }
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL)) {
        perror("sigaction");
        goto out;
    }
    request.offsets[0] = info.offset;
    request.num_lines = 1;
    request.event_buffer_size = 32;
    request.config.flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_EDGE_RISING |
                           GPIO_V2_LINE_FLAG_EDGE_FALLING;
    snprintf(request.consumer, sizeof(request.consumer), "w202-hall-events");
    if (ioctl(chip_fd, GPIO_V2_GET_LINE_IOCTL, &request)) {
        perror("request Hall input/edge events");
        goto out;
    }
    event_fd = request.fd;
    int file_flags = fcntl(event_fd, F_GETFL);
    if (file_flags < 0 || fcntl(event_fd, F_SETFL, file_flags | O_NONBLOCK) ||
        fcntl(event_fd, F_SETFD, FD_CLOEXEC) ||
        ioctl(event_fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &values)) {
        perror("configure/read Hall input");
        goto out;
    }
    deadline = now_ns();
    if (deadline < 0) {
        perror("clock_gettime");
        goto out;
    }
    deadline += seconds * INT64_C(1000000000);
    printf("{\"event\":\"ready\",\"raw\":%u,\"seconds\":%ld,\"polarity_verified\":false}\n",
           (unsigned)(values.bits & 1), seconds);
    while (!interrupted) {
        int64_t current = now_ns();
        struct timespec timeout;
        struct pollfd poll_fd = {.fd = event_fd, .events = POLLIN};
        if (current < 0) {
            perror("clock_gettime");
            goto out;
        }
        if (current >= deadline)
            break;
        int64_t remaining = deadline - current;
        timeout.tv_sec = remaining / 1000000000;
        timeout.tv_nsec = remaining % 1000000000;
        int ready = ppoll(&poll_fd, 1, &timeout, NULL);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            perror("ppoll Hall events");
            goto out;
        }
        if (!ready)
            break;
        if (poll_fd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
            fprintf(stderr, "Hall event descriptor failed\n");
            goto out;
        }
        struct gpio_v2_line_event batch[16];
        ssize_t got = read(event_fd, batch, sizeof(batch));
        if (got < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (got <= 0 || got % sizeof(batch[0])) {
            fprintf(stderr, "short/failed GPIO event read: %zd\n", got);
            goto out;
        }
        for (size_t i = 0; i < (size_t)got / sizeof(batch[0]); i++) {
            const struct gpio_v2_line_event *event = &batch[i];
            if (event->offset != 148 ||
                (event->id != GPIO_V2_LINE_EVENT_RISING_EDGE &&
                 event->id != GPIO_V2_LINE_EVENT_FALLING_EDGE)) {
                fprintf(stderr, "unexpected GPIO event\n");
                goto out;
            }
            if (event->line_seqno != previous_seq + 1)
                missed += event->line_seqno - previous_seq - 1;
            previous_seq = event->line_seqno;
            events++;
            printf("{\"event\":\"edge\",\"raw\":%u,\"sequence\":%u,\"monotonic_ns\":%" PRIu64 "}\n",
                   event->id == GPIO_V2_LINE_EVENT_RISING_EDGE ? 1u : 0u,
                   event->line_seqno, (uint64_t)event->timestamp_ns);
        }
    }
    printf("{\"event\":\"done\",\"edges\":%u,\"missed\":%u,\"signal\":%d}\n",
           events, missed, (int)interrupted);
    result = missed ? 1 : 0;
out:
    if (event_fd >= 0)
        close(event_fd);
    if (chip_fd >= 0)
        close(chip_fd);
    return result;
}
