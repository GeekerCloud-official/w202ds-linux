// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <drm/panfrost_drm.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../source/w202_vpu_buffer_uapi.h"

#define BUFFER_SIZE 65536
static void check(int okay, const char *what)
{
	if (!okay) {
		fprintf(stderr, "FAIL %s: %s\n", what, strerror(errno));
		exit(1);
	}
}

static int export_gpu_buffer(void)
{
	struct drm_panfrost_create_bo bo = { .size = BUFFER_SIZE };
	struct drm_prime_handle prime = { .flags = DRM_CLOEXEC | DRM_RDWR };
	struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
	int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	void *cpu;

	check(render >= 0, "open GPU render device");
	check(ioctl(render, DRM_IOCTL_PANFROST_CREATE_BO, &bo) == 0, "create GPU BO");
	prime.handle = bo.handle;
	check(ioctl(render, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) == 0, "export GPU DMA-BUF");
	close(render); /* GPU handle/context no longer owns this buffer. */
	cpu = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, prime.fd, 0);
	check(cpu != MAP_FAILED, "mmap exported DMA-BUF");
	check(ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU access begin");
	memset(cpu, 0x3c, BUFFER_SIZE);
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
	check(ioctl(prime.fd, DMA_BUF_IOCTL_SYNC, &sync) == 0, "CPU access end");
	check(munmap(cpu, BUFFER_SIZE) == 0, "unmap CPU view");
	return prime.fd;
}

static void expect_error(int file, unsigned long cmd, void *arg, int expected, const char *what)
{
	errno = 0;
	check(ioctl(file, cmd, arg) == -1 && errno == expected, what);
}

static void test_core(const char *node)
{
	struct w202_vpu_iova first = {}, second = {}, invalid = { .fd = -1 };
	struct w202_vpu_iova *readonly;
	int a = open(node, O_RDWR | O_CLOEXEC);
	int b = open(node, O_RDWR | O_CLOEXEC);
	int dmabuf = export_gpu_buffer(), alias = dup(dmabuf), ready[2];
	pid_t child;
	int status;
	char byte;

	check(a >= 0 && b >= 0 && alias >= 0, "open independent contexts / duplicate DMA-BUF");
	expect_error(a, W202_VPU_GET_IOVA, &invalid, EINVAL, "negative fd rejected");
	expect_error(a, _IO('m', 3), NULL, ENOTTY, "unsupported codec ioctl rejected");
	readonly = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(readonly != MAP_FAILED, "allocate copyout failure input");
	readonly->fd = dmabuf;
	check(mprotect(readonly, 4096, PROT_READ) == 0, "make ioctl output read-only");
	expect_error(a, W202_VPU_GET_IOVA, readonly, EFAULT, "copyout failure rolled back");
	munmap(readonly, 4096);

	first.fd = dmabuf;
	second.fd = alias;
	second.need_cache_sync = 1;
	check(ioctl(a, W202_VPU_GET_IOVA, &first) == 0, "map DMA-BUF (cache flag 0)");
	check(ioctl(a, W202_VPU_GET_IOVA, &second) == 0, "map aliased DMA-BUF (cache flag 1)");
	check(first.size == BUFFER_SIZE && second.size == BUFFER_SIZE &&
	      first.iova_addr != second.iova_addr, "distinct imports have distinct addresses");
	close(dmabuf);
	close(alias); /* Only importer references remain. */
	expect_error(b, W202_VPU_FREE_IOVA, &first, ENOENT, "cross-context release rejected");
	first.fd = -1;
	first.need_cache_sync = 255;
	check(ioctl(a, W202_VPU_FREE_IOVA, &first) == 0, "free after original fd close using saved metadata");
	expect_error(a, W202_VPU_FREE_IOVA, &first, ENOENT, "double release rejected");
	close(a); /* Second mapping must be cleaned automatically. */
	close(b);
	printf("%s aliases/closed-fd/free/cross-context/copyout/close PASS; IOVA=%#llx,%#llx\n",
	       node, (unsigned long long)first.iova_addr, (unsigned long long)second.iova_addr);

	check(pipe2(ready, O_CLOEXEC) == 0, "create child readiness pipe");
	child = fork();
	check(child >= 0, "fork mapping owner");
	if (!child) {
		struct w202_vpu_iova m = {};
		close(ready[0]);
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		if (getppid() == 1)
			_exit(2);
		a = open(node, O_RDWR | O_CLOEXEC);
		check(a >= 0, "child open context");
		m.fd = export_gpu_buffer();
		check(ioctl(a, W202_VPU_GET_IOVA, &m) == 0, "child import");
		close(m.fd);
		check(write(ready[1], "R", 1) == 1, "signal mapping is live");
		for (;;)
			pause();
	}
	close(ready[1]);
	alarm(15);
	check(read(ready[0], &byte, 1) == 1 && byte == 'R', "child mapping ready");
	close(ready[0]);
	check(kill(child, SIGKILL) == 0, "kill mapping owner");
	check(waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
	      WTERMSIG(status) == SIGKILL, "wait for killed owner cleanup");
	alarm(0);
	printf("%s killed-owner cleanup completed\n", node);
}

int main(void)
{
	static const char *const nodes[] = {
		"/dev/w202-vpu-map-enc0", "/dev/w202-vpu-map-enc1", "/dev/w202-vpu-map-dec",
	};
	unsigned int i;

	_Static_assert(sizeof(struct w202_vpu_iova) == 32, "vendor-compatible ABI size");
	setvbuf(stdout, NULL, _IONBF, 0);
	for (i = 0; i < sizeof(nodes) / sizeof(nodes[0]); i++)
		test_core(nodes[i]);
	puts("THREE_CORE_REAL_GPU_DMABUF_OWNERSHIP_USERSPACE_PASS");
	return 0;
}
