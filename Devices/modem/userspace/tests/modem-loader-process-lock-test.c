/* SPDX-License-Identifier: MIT
 * Original reopen-per-operation ABI. Only lock/unlock ioctls; no modem memory
 * or firmware start/stop, and debugfs reads do not reap an exited owner.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static unsigned long lock_cmd, unlock_cmd;
static const char *direction;
static unsigned int passed;

static void check(bool ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s: %s errno=%d (%s)\n", direction, message, errno, strerror(errno));
        exit(1);
    }
}

static void pass(const char *message)
{
    printf("PASS %s: %s\n", direction, message);
    ++passed;
}

static int open_node(bool nonblock)
{
    struct stat st;
    int fd = open("/dev/modem", O_RDWR | O_CLOEXEC | (nonblock ? O_NONBLOCK : 0));
    check(fd >= 0 && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode), "open character node");
    return fd;
}

static void expect(int fd, unsigned long cmd, int error)
{
    errno = 0;
    check(ioctl(fd, cmd) == -1 && errno == error, "expected ioctl denial");
}

static void send_byte(int fd, unsigned char value)
{
    check(write(fd, &value, 1) == 1, "pipe write");
}

static int get_byte(int fd)
{
    struct pollfd p = {.fd = fd, .events = POLLIN};
    unsigned char value;
    check(poll(&p, 1, 4000) == 1 && (p.revents & POLLIN), "pipe response within deadline");
    check(read(fd, &value, 1) == 1, "pipe read");
    return value;
}

static void wait_child(pid_t child, bool killed)
{
    int status;
    check(waitpid(child, &status, 0) == child, "wait child");
    check(killed ? WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL
                 : WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit status");
}

static int debug_owner(void)
{
    FILE *file = fopen("/sys/kernel/debug/modem/modem", "r");
    char line[256], key[32];
    int owner = -1;
    check(file != NULL, "open debug state");
    snprintf(key, sizeof(key), "%s_owner:", direction);
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, strlen(key)) == 0) {
            check(sscanf(line + strlen(key), "%d", &owner) == 1, "parse debug owner");
            break;
        }
    }
    fclose(file);
    check(owner >= 0, "owner field present");
    return owner;
}

static void expect_exit_cleanup(void)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (debug_owner() == 0) return;
        usleep(1000);
    }
    check(false, "exit notification releases lock without a new acquire");
}

static void foreign_denied(int inherited)
{
    pid_t child = fork();
    check(child >= 0, "fork foreign process");
    if (child == 0) {
        int fd = open_node(true);
        alarm(5);
        /* The comm matches the parent; a task name is not ownership. */
        check(prctl(PR_SET_NAME, "loader-test", 0, 0, 0) == 0, "set same comm");
        expect(fd, unlock_cmd, EPERM);
        expect(fd, lock_cmd, EBUSY);
        if (inherited >= 0) expect(inherited, unlock_cmd, EPERM);
        close(fd);
        _exit(0);
    }
    wait_child(child, false);
}

static void test_reopen(void)
{
    int fd = open_node(true);
    unsigned long other_lock = lock_cmd == _IO('M', 1) ? _IO('M', 3) : _IO('M', 1);
    unsigned long other_unlock = lock_cmd == _IO('M', 1) ? _IO('M', 4) : _IO('M', 2);
    check(ioctl(fd, lock_cmd) == 0, "initial lock");
    check(debug_owner() == getpid(), "process owner recorded");
    close(fd);
    fd = open_node(true);
    check(debug_owner() == getpid(), "close/reopen preserves original ABI");
    expect(fd, lock_cmd, EBUSY);
    check(ioctl(fd, other_lock) == 0 && ioctl(fd, other_unlock) == 0, "independent direction");
    foreign_denied(fd);
    check(ioctl(fd, unlock_cmd) == 0, "unlock through reopened fd");
    expect(fd, unlock_cmd, EPERM);
    close(fd);
    pass("reopen ABI, owner identity, foreign/inherited fd denial, independent gates");
}

static void test_exit(bool killed)
{
    int ready[2], finish[2], fd;
    pid_t child;
    check(pipe2(ready, O_CLOEXEC) == 0 && pipe2(finish, O_CLOEXEC) == 0, "holder pipes");
    child = fork();
    check(child >= 0, "fork holder");
    if (child == 0) {
        close(ready[0]); close(finish[1]);
        alarm(5);
        fd = open_node(false);
        check(ioctl(fd, lock_cmd) == 0, "child lock");
        close(fd);
        send_byte(ready[1], 1);
        check(get_byte(finish[0]) == 1, "finish holder");
        _exit(0);
    }
    close(ready[1]); close(finish[0]);
    check(get_byte(ready[0]) == 1, "holder ready");
    check(debug_owner() == child, "owner survives holder fd close");
    fd = open_node(true);
    expect(fd, lock_cmd, EBUSY);
    if (killed) check(kill(child, SIGKILL) == 0, "kill holder");
    else send_byte(finish[1], 1);
    wait_child(child, killed);
    expect_exit_cleanup();
    check(ioctl(fd, lock_cmd) == 0 && ioctl(fd, unlock_cmd) == 0, "acquire after exit cleanup");
    close(fd); close(ready[0]); close(finish[1]);
    pass(killed ? "SIGKILL event cleanup without new acquire" : "normal exit event cleanup without new acquire");
}

static void signal_handler(int signal_number) { (void)signal_number; }

static void test_interrupt(void)
{
    int owner = open_node(true), channel[2];
    pid_t child;
    check(ioctl(owner, lock_cmd) == 0, "hold for interruption");
    check(pipe2(channel, O_CLOEXEC) == 0, "interrupt pipe");
    child = fork();
    check(child >= 0, "fork waiter");
    if (child == 0) {
        struct sigaction action = {.sa_handler = signal_handler};
        int fd, result, saved;
        close(owner); close(channel[0]);
        alarm(5);
        sigemptyset(&action.sa_mask);
        check(sigaction(SIGUSR1, &action, NULL) == 0, "non-restarting signal");
        fd = open_node(false);
        send_byte(channel[1], 1);
        errno = 0; result = ioctl(fd, lock_cmd); saved = errno;
        send_byte(channel[1], result == -1 && saved == EINTR ? 2 : 3);
        close(fd);
        _exit(0);
    }
    close(channel[1]);
    check(get_byte(channel[0]) == 1, "waiter ready");
    struct pollfd p = {.fd = channel[0], .events = POLLIN};
    for (int attempt = 0; attempt < 200; ++attempt) {
        check(kill(child, SIGUSR1) == 0 || errno == ESRCH, "signal waiter");
        if (poll(&p, 1, 10) == 1) break;
    }
    check(get_byte(channel[0]) == 2, "blocked ioctl interrupted");
    wait_child(child, false);
    check(debug_owner() == getpid(), "waiter interruption preserves owner");
    check(ioctl(owner, unlock_cmd) == 0, "release after interruption");
    close(owner); close(channel[0]);
    pass("blocking waiter interrupted, original owner preserved");
}

static void test_exit_wakes_waiter(void)
{
    int holder_channel[2], waiter_channel[2];
    pid_t holder, waiter;
    check(pipe2(holder_channel, O_CLOEXEC) == 0 && pipe2(waiter_channel, O_CLOEXEC) == 0, "wake pipes");
    holder = fork();
    check(holder >= 0, "fork event holder");
    if (holder == 0) {
        int fd = open_node(false);
        alarm(5);
        close(holder_channel[0]); close(waiter_channel[0]); close(waiter_channel[1]);
        check(ioctl(fd, lock_cmd) == 0, "event holder lock");
        close(fd);
        send_byte(holder_channel[1], 1);
        for (;;) pause();
    }
    close(holder_channel[1]);
    check(get_byte(holder_channel[0]) == 1, "event holder ready");
    waiter = fork();
    check(waiter >= 0, "fork event waiter");
    if (waiter == 0) {
        int fd = open_node(false);
        alarm(5);
        close(holder_channel[0]); close(waiter_channel[0]);
        send_byte(waiter_channel[1], 1);
        check(ioctl(fd, lock_cmd) == 0, "acquire after owner death");
        check(ioctl(fd, unlock_cmd) == 0, "event waiter release");
        send_byte(waiter_channel[1], 2);
        close(fd);
        _exit(0);
    }
    close(waiter_channel[1]);
    check(get_byte(waiter_channel[0]) == 1, "event waiter ready");
    struct pollfd p = {.fd = waiter_channel[0], .events = POLLIN};
    check(poll(&p, 1, 50) == 0, "waiter blocks while owner alive");
    check(kill(holder, SIGKILL) == 0, "kill event holder");
    wait_child(holder, true);
    check(get_byte(waiter_channel[0]) == 2, "exit event wakes waiter");
    wait_child(waiter, false);
    close(holder_channel[0]); close(waiter_channel[0]);
    pass("owner exit wakes blocked acquire");
}

struct thread_op { int fd, result, error; bool unlock; pthread_barrier_t *barrier; };
static void *thread_ioctl(void *argument)
{
    struct thread_op *op = argument;
    if (op->barrier) pthread_barrier_wait(op->barrier);
    errno = 0;
    op->result = ioctl(op->fd, op->unlock ? unlock_cmd : lock_cmd);
    op->error = errno;
    return NULL;
}

static void test_threads(void)
{
    int fd = open_node(false), second = open_node(false);
    pthread_t threads[2];
    pthread_barrier_t barrier;
    struct thread_op op[2] = {{.fd = fd, .barrier = &barrier}, {.fd = second, .barrier = &barrier}};
    check(pthread_barrier_init(&barrier, NULL, 3) == 0, "race barrier");
    for (int i = 0; i < 2; ++i) check(pthread_create(&threads[i], NULL, thread_ioctl, &op[i]) == 0, "race thread");
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 2; ++i) check(pthread_join(threads[i], NULL) == 0, "join race");
    check((op[0].result == 0 && op[1].result == -1 && op[1].error == EBUSY) ||
          (op[1].result == 0 && op[0].result == -1 && op[0].error == EBUSY), "one winner, one busy");
    check(debug_owner() == getpid(), "worker thread exit retains process owner");
    op[0] = (struct thread_op){.fd = second, .unlock = true};
    check(pthread_create(&threads[0], NULL, thread_ioctl, &op[0]) == 0 &&
          pthread_join(threads[0], NULL) == 0 && op[0].result == 0, "cross-thread other-fd unlock");
    close(fd); close(second); pthread_barrier_destroy(&barrier);
    pass("same-process concurrent acquisition and cross-thread reopen unlock");
}

struct leader_worker { int ready, finish; };
static void *leader_worker_main(void *argument)
{
    struct leader_worker *channels = argument;
    int fd = open_node(false);
    check(ioctl(fd, lock_cmd) == 0, "worker acquires group lease");
    close(fd);
    send_byte(channels->ready, 1);
    check(get_byte(channels->finish) == 1, "worker finishes after leader exit");
    return NULL;
}

static void test_early_leader_exit(void)
{
    int ready[2], finish[2], fd;
    pid_t child;
    check(pipe2(ready, O_CLOEXEC) == 0 && pipe2(finish, O_CLOEXEC) == 0, "leader pipes");
    child = fork();
    check(child >= 0, "fork thread group");
    if (child == 0) {
        pthread_t worker;
        struct leader_worker *channels = malloc(sizeof(*channels));
        check(channels != NULL, "allocate group channels");
        close(ready[0]); close(finish[1]); alarm(5);
        *channels = (struct leader_worker){.ready = ready[1], .finish = finish[0]};
        check(pthread_create(&worker, NULL, leader_worker_main, channels) == 0, "create group worker");
        pthread_exit(NULL);
    }
    close(ready[1]); close(finish[0]);
    check(get_byte(ready[0]) == 1, "group worker holds lease");
    usleep(20000);
    check(debug_owner() == child, "leader exit keeps live group's ownership");
    fd = open_node(true);
    expect(fd, lock_cmd, EBUSY);
    send_byte(finish[1], 1);
    wait_child(child, false);
    expect_exit_cleanup();
    check(ioctl(fd, lock_cmd) == 0 && ioctl(fd, unlock_cmd) == 0, "last group thread releases lease");
    close(fd); close(ready[0]); close(finish[1]);
    pass("leader exit preserves lease until last group thread exits");
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "/dev/modem") != 0) {
        fprintf(stderr, "Usage: %s /dev/modem\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    check(prctl(PR_SET_NAME, "loader-test", 0, 0, 0) == 0, "set process comm");
    alarm(30);
    for (int read_direction = 1; read_direction >= 0; --read_direction) {
        direction = read_direction ? "read" : "write";
        lock_cmd = read_direction ? _IO('M', 1) : _IO('M', 3);
        unlock_cmd = read_direction ? _IO('M', 2) : _IO('M', 4);
        test_reopen();
        test_exit(false);
        test_exit(true);
        test_interrupt();
        test_exit_wakes_waiter();
        test_threads();
        test_early_leader_exit();
    }
    printf("PASS total=%u; no firmware I/O or start/stop executed\n", passed);
    return 0;
}
