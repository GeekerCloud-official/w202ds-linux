// SPDX-License-Identifier: GPL-2.0-only
/* Native decoder lifecycle acceptance. No raw register writes or codec jobs.
 * Actual decoding is independently checked with test-vendor-avc and FFmpeg.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <drm/panfrost_drm.h>
#include "../../source/w202_vpu_buffer_uapi.h"

#define CMD_FREQ _IOW('m', 1, unsigned int)
#define CMD_ENABLE _IO('m', 3)
#define CMD_DISABLE _IO('m', 4)
#define CMD_ACQUIRE _IO('m', 5)
#define CMD_RELEASE _IO('m', 6)
#define CMD_COMPLETE _IO('m', 7)
#define CMD_HW_INFO _IO('m', 9)
#define CMD_VERSION _IO('m', 10)
#define CMD_IOMMU _IO('m', 13)
#define DEVICE "/dev/sprd_vsp"
#define STATE "/sys/bus/platform/devices/32200000.video-codec/state"

static void check(int ok, const char *message)
{
    if (!ok) { fprintf(stderr, "FAIL %s errno=%d (%s)\n", message, errno, strerror(errno)); exit(1); }
}

static unsigned int field(const char *name)
{
    char text[2048], key[64];
    int fd = open(STATE, O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "open codec state");
    ssize_t count = read(fd, text, sizeof(text) - 1);
    check(count > 0, "read codec state");
    close(fd);
    text[count] = 0;
    snprintf(key, sizeof(key), "%s=", name);
    char *start = strstr(text, key);
    check(start != NULL, "codec state field exists");
    return strtoul(start + strlen(key), NULL, 10);
}

static void expect_error(int result, int expected, const char *message)
{
    check(result == -1 && errno == expected, message);
}

static void denied_map(int fd, size_t size, int prot, int flags, off_t offset)
{
    void *address = mmap(NULL, size, prot, flags, fd, offset);
    check(address == MAP_FAILED && errno == EINVAL, "invalid MMIO mapping rejected");
}

static int buffer_fd(void)
{
    struct drm_panfrost_create_bo bo = {.size = 65536};
    struct drm_prime_handle prime = {.flags = DRM_CLOEXEC | DRM_RDWR};
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    check(fd >= 0, "open real GPU exporter");
    check(!ioctl(fd, DRM_IOCTL_PANFROST_CREATE_BO, &bo), "allocate GPU buffer");
    prime.handle = bo.handle;
    check(!ioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime), "export real DMA-BUF");
    close(fd);
    return prime.fd;
}

static long long now_ms(void)
{
    struct timespec ts;
    check(!clock_gettime(CLOCK_MONOTONIC, &ts), "read monotonic clock");
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void pause_ms(unsigned int milliseconds)
{
    struct timespec delay = {.tv_sec = milliseconds / 1000,
                            .tv_nsec = (milliseconds % 1000) * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

static unsigned int pmu(const char *name)
{
    char text[256], key[32];
    int fd = open("/sys/module/w202_vpu_power/parameters/state", O_RDONLY | O_CLOEXEC);
    check(fd >= 0, "open real PMU status");
    ssize_t count = read(fd, text, sizeof(text) - 1);
    check(count > 0, "read real PMU status");
    close(fd);
    text[count] = 0;
    snprintf(key, sizeof(key), "%s=", name);
    char *start = strstr(text, key);
    check(start != NULL, "PMU state field exists");
    return strtoul(start + strlen(key), NULL, 16);
}

static void idle_power(void)
{
    long long begin = now_ms();
    while (!pmu("enc0") || !pmu("enc1") || !pmu("dec")) {
        check(now_ms() - begin < 2000, "all unused VPU children power off within bound");
        pause_ms(20);
    }
    check(!pmu("top") && !field("powered"), "shared display on and decoder owner off");
    printf("REAL_PMU_IDLE_OFF_PASS observed_ms=%lld\n", now_ms() - begin);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    check(!field("opens") && !field("maps") && !field("broken"), "initial idle healthy state");
    idle_power();
    unsigned int released = field("auto_released"), rolled = field("copyout_rollbacks");
    unsigned int faults = field("faults");
    int dma = buffer_fd();
    int fd = open(DEVICE, O_RDWR | O_CLOEXEC);
    check(fd >= 0, "exclusive owner opens");
    pause_ms(350);
    check(!pmu("dec") && pmu("enc0") && pmu("enc1") && field("powered"),
          "owner holds only decoder power beyond autosuspend delay");
    expect_error(open(DEVICE, O_RDWR | O_CLOEXEC), EBUSY, "second owner denied");
    uint32_t value = 0;
    check(!ioctl(fd, CMD_VERSION, &value) && value == 0x15, "native Qogirn6Pro version");
    check(!ioctl(fd, CMD_HW_INFO, &value) && value == (1U << 21), "native domain clock info");
    check(!ioctl(fd, CMD_IOMMU, NULL), "real VAU domain attached");
    expect_error(ioctl(fd, _IO('m', 127), NULL), ENOTTY, "unsupported ioctl rejected");
    expect_error(ioctl(fd, CMD_ENABLE, NULL), EPERM, "enable requires acquire");
    expect_error(ioctl(fd, CMD_COMPLETE, &value), EPERM, "completion requires enabled owner");
    denied_map(fd, 0x15000, PROT_READ | PROT_WRITE, MAP_SHARED, 0);
    denied_map(fd, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, 4096);
    denied_map(fd, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE, 0);
    denied_map(fd, 0x1000, PROT_READ | PROT_EXEC, MAP_SHARED, 0);
    void *registers = mmap(NULL, 0x14000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(registers != MAP_FAILED, "native full ABI MMIO mapping");
    expect_error(mprotect(registers, 0x1000, PROT_READ | PROT_EXEC), EACCES, "MMIO cannot gain execution");

    struct w202_vpu_iova map = {.fd = dma, .need_cache_sync = 1};
    check(!ioctl(fd, W202_VPU_GET_IOVA, &map), "DMA-BUF imported");
    check(map.size == 65536 && map.iova_addr >= 0x20000000 && map.iova_addr + map.size <= 0x60000000,
          "mapped DMA-BUF in decoder aperture");
    check(field("maps") == 1, "live map accounted");
    struct w202_vpu_iova *readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(readonly != MAP_FAILED, "allocate copyout error input");
    *readonly = (struct w202_vpu_iova){.fd = dma};
    check(!mprotect(readonly, 4096, PROT_READ), "protect output argument");
    expect_error(ioctl(fd, W202_VPU_GET_IOVA, readonly), EFAULT, "failed copyout rolled back");
    check(field("maps") == 1 && field("copyout_rollbacks") == rolled + 1, "copyout rollback preserved map count");
    munmap(readonly, 4096);
    struct w202_vpu_iova wrong = map;
    wrong.iova_addr += 4096;
    expect_error(ioctl(fd, W202_VPU_FREE_IOVA, &wrong), ENOENT, "unowned IOVA rejected");
    check(!ioctl(fd, CMD_ACQUIRE, NULL), "acquire owner");
    expect_error(ioctl(fd, CMD_ACQUIRE, NULL), EALREADY, "double acquire denied");
    value = 5;
    expect_error(ioctl(fd, CMD_FREQ, &value), EINVAL, "out of range clock denied");
    value = 0;
    check(!ioctl(fd, CMD_FREQ, &value) && !ioctl(fd, CMD_ENABLE, NULL), "enable at supported low clock");
    value = 0;
    long long begin = now_ms();
    expect_error(ioctl(fd, CMD_COMPLETE, &value), ETIMEDOUT, "absent interrupt has bounded timeout");
    long long elapsed = now_ms() - begin;
    check(value == (1U << 31) && elapsed >= 180 && elapsed < 1500, "native timeout status and duration");
    check(!ioctl(fd, CMD_DISABLE, NULL) && !ioctl(fd, CMD_RELEASE, NULL), "stop resets and releases");
    check(!close(fd), "close numeric fd while VMA lives");
    pause_ms(350);
    expect_error(open(DEVICE, O_RDWR | O_CLOEXEC), EBUSY, "MMIO VMA retains exclusive ownership");
    check(field("opens") == 1 && field("maps") == 1, "MMIO VMA retains import context");
    check(!pmu("dec") && field("powered"), "live VMA retains actual decoder power");
    check(!munmap(registers, 0x14000), "last VMA release quiesces decoder");
    check(!field("opens") && !field("maps") && field("auto_released") == released + 1,
          "last VMA released orphaned DMA-BUF import");
    printf("API_OWNERSHIP_MMAP_COPYOUT_TIMEOUT_PASS elapsed_ms=%lld\n", elapsed);
    idle_power();

    int ready[2];
    check(!pipe2(ready, O_CLOEXEC), "create SIGKILL synchronization pipe");
    pid_t child = fork();
    check(child >= 0, "fork owner for fatal signal acceptance");
    if (!child) {
        close(ready[0]);
        int owned = open(DEVICE, O_RDWR | O_CLOEXEC);
        check(owned >= 0, "child owns codec");
        struct w202_vpu_iova imported = {.fd = dma};
        check(!ioctl(owned, W202_VPU_GET_IOVA, &imported), "child imports real buffer");
        close(dma);
        check(!ioctl(owned, CMD_ACQUIRE, NULL) && !ioctl(owned, CMD_ENABLE, NULL), "child enabled codec");
        check(write(ready[1], "R", 1) == 1, "child ready to await IRQ");
        ioctl(owned, CMD_COMPLETE, &value);
        _exit(2);
    }
    close(ready[1]);
    char token = 0;
    check(read(ready[0], &token, 1) == 1 && token == 'R', "child enabled and imported");
    close(ready[0]);
    pause_ms(20);
    check(field("opens") == 1 && field("maps") == 1, "child holds codec and import");
    check(!kill(child, SIGKILL), "kill owner awaiting hardware completion");
    int status;
    check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "child exited by SIGKILL");
    check(!field("opens") && !field("maps") && field("auto_released") == released + 2,
          "fatal signal reset decoder and freed import");
    check(!field("broken") && !field("quarantined") && field("faults") == faults,
          "lifecycle acceptance left healthy hardware");
    close(dma);
    idle_power();
    printf("API_SIGKILL_ENABLED_OWNER_PASS\nCODEC_LIFECYCLE_ACCEPTANCE_PASS\n");
    return 0;
}
