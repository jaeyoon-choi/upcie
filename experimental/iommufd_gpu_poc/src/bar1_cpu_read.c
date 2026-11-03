// SPDX-License-Identifier: BSD-3-Clause
// Write a pattern into a CUDA buffer, then read the same bytes through the
// GPU's BAR1 (sysfs resource1) from the CPU and compare. Needs root.
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cuda.h>
#include "dmabuf_import.h"
#ifndef GPU_BDF
#define GPU_BDF "0000:31:00.0"
#endif
int main(void)
{
	size_t size = 1ULL << 30, n = 65536;
	CUdevice d; CUcontext ctx; CUdeviceptr p; int fd;
	cuInit(0); cuDeviceGet(&d, 0); cuCtxCreate(&ctx, 0, d);
	cuMemAlloc(&p, size);
	cuMemGetHandleForAddressRange(&fd, p, size, CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD, 0);
	uint8_t *pat = malloc(n), *got = malloc(n);
	for (size_t i = 0; i < n; i++) pat[i] = (uint8_t)(i * 7 + 3);
	cuMemcpyHtoD(p, pat, n); cuCtxSynchronize();
	int di = open(DMABUF_IMPORT_DEVPATH, O_RDWR);
	struct dmabuf_import_attach_bdf at = { .fd = fd, .bdf = "" };
	ioctl(di, DMABUF_IMPORT_ATTACH_BDF, &at);
	struct dmabuf_import_info info = { .fd = fd };
	ioctl(di, DMABUF_IMPORT_GET_INFO, &info);
	struct dmabuf_import_get_map *gm = calloc(1, sizeof(*gm) + info.count * sizeof(gm->dma_arr[0]));
	gm->fd = fd; gm->count = info.count;
	ioctl(di, DMABUF_IMPORT_GET_MAP, gm);
	uint64_t pa = gm->dma_arr[0].dma_addr;
	FILE *f = fopen("/sys/bus/pci/devices/" GPU_BDF "/resource", "r");
	unsigned long long s0, e0, f0, s1, e1, f1;
	fscanf(f, "%llx %llx %llx %llx %llx %llx", &s0, &e0, &f0, &s1, &e1, &f1);
	int r1 = open("/sys/bus/pci/devices/" GPU_BDF "/resource1", O_RDWR | O_SYNC);
	uint64_t off = pa - s1, pg = off & ~0xfffULL;
	volatile uint8_t *m = mmap(NULL, n + 4096, PROT_READ, MAP_SHARED, r1, pg);
	if (m == MAP_FAILED) { perror("mmap resource1"); return 1; }
	for (size_t i = 0; i < n; i++) got[i] = m[off - pg + i];
	printf("bar1 base 0x%llx, buffer pa 0x%llx, offset 0x%llx\n", s1, (unsigned long long)pa, (unsigned long long)off);
	printf("CPU read through BAR1: %s\n", memcmp(got, pat, n) ? "MISMATCH" : "MATCH");
	printf("first bytes got %02x %02x %02x %02x want %02x %02x %02x %02x\n", got[0], got[1], got[2], got[3], pat[0], pat[1], pat[2], pat[3]);
	return 0;
}
