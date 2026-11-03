# gpu_gmem_provider (PoC)

A guest_memfd PFN mode provider backed by an NVIDIA dma-buf. With it, a
process that runs no guest can wrap CUDA memory in a guest_memfd and map that
guest_memfd into an NVMe device's IOMMU with `IOMMU_IOAS_MAP_FILE`. It was
written for the iommufd GPU mapping proof of concept and is not maintained.
Nothing here is upstream, and the kernel interfaces it uses are RFCs that
will change.

## What it needs

| Part | Version |
| --- | --- |
| Kernel | kvm.git `7e60162b16f9` (2026-10-22, now part of v7.4-rc1) with Ackerley Tng's [guest_memfd alternative providers RFC v2](https://lore.kernel.org/all/20261022-gmem-alternative-providers-v2-0-5dd1dc6c660a@google.com/) (50 patches) and Fred Griffoul's [guest_memfd export RFC](https://lore.kernel.org/all/20261027154649.91571-1-griffoul@gmail.com/) (14 patches). Tree `a6af7e15843c`. A git bundle of this tree on top of v7.3-rc7 is in [`notes/pal-poc/patches/`](https://github.com/jaeyoon-choi/aisio/tree/poc/iommufd-pal/notes/pal-poc/patches) of the aisio fork |
| Kernel config | `KVM=m`, `KVM_INTEL=m` (selects `KVM_GUEST_MEMFD`), `IOMMUFD=y`, `VFIO_DEVICE_CDEV=y` |
| NVIDIA driver | 580.178.04 open kernel modules with `nv_dma_buf_get_bar1_ranges()`, branch [`poc/gmem-bar1-ranges`](https://github.com/jaeyoon-choi/open-gpu-kernel-modules/tree/poc/gmem-bar1-ranges) |

## Build

```
make -C module KDIR=/path/to/gmem-kernel NVSYMVERS=/path/to/open-gpu-kernel-modules/kernel-open/Module.symvers
```

## How it is used

1. Export CUDA memory as a dma-buf (`cuMemGetHandleForAddressRange()`).
2. Pass the dma-buf fd to `/dev/gpu_gmem_provider` with `GGP_NEW_FILE`
   (`module/gpu_gmem_provider.h`). The module reads the BAR1 ranges of the
   dma-buf and returns a provider file.
3. Create an empty VM (`KVM_CREATE_VM`, no vCPU, no memslot) and call
   `KVM_CREATE_GUEST_MEMFD` on it with `GUEST_MEMFD_FLAG_INIT_SHARED`,
   `GUEST_MEMFD_FLAG_USE_RESOURCE`, `GUEST_MEMFD_FLAG_PFN_MODE` and the
   provider file as `resource_fd`.
4. Map the guest_memfd with `IOMMU_IOAS_MAP_FILE`.

`pal_nvme --gmem` in [`../iommufd_gpu_poc`](../iommufd_gpu_poc) does these
steps.

## Limits

- One provider file backs one guest_memfd.
- The BAR1 ranges stay fixed for the life of the file, so the provider never
  revokes or restores frames. NVIDIA does not tell importers when it would
  move or free the memory, and the memory stays allocated until the mapping
  is gone.
- The RFC's iommufd code maps guest_memfd with 4 KiB IOPTEs. A 1 GiB buffer
  takes about 9 ms to map on the test server, against 34.6 µs for
  `iommu_map_pa`. `get_pfn` already reports the largest aligned block, so
  larger IOPTEs need no change here once iommufd supports them.
