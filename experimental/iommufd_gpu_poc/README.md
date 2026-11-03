# iommufd GPU mapping PoC tools

Tools from the proof of concept that mapped CUDA memory into an NVMe
device's IOMMU through iommufd's `IOMMU_IOAS_MAP_FILE`, instead of with
`iommu_map_pa`. Upstream iommufd rejects NVIDIA dma-bufs, so the PoC ran on
two RFC kernels. The tools are kept as the PoC left them and are not
maintained. The report, the server manuals, the measurements and the raw
logs are on the aisio fork branch
[`poc/iommufd-pal`](https://github.com/jaeyoon-choi/aisio/tree/poc/iommufd-pal/notes/pal-poc).

## Kernels and drivers

No kernel code was changed. The PoC built these trees as they are.

| PoC | Kernel | NVIDIA 580.178.04 | upcie |
| --- | --- | --- | --- |
| PAL | `jgunthorpe/linux` branch `dmabuf_map_type`, commit `c37cf2824740` (on v7.0-rc1 `6de23f81a5e0`) | [`poc/iommufd-pal-exporter`](https://github.com/jaeyoon-choi/open-gpu-kernel-modules/tree/poc/iommufd-pal-exporter) | `dmabuf_import` from this branch (builds on the mapping types series) |
| guest_memfd export | kvm.git `7e60162b16f9` with Ackerley Tng's [alternative providers RFC v2](https://lore.kernel.org/all/20261022-gmem-alternative-providers-v2-0-5dd1dc6c660a@google.com/) and Fred Griffoul's [guest_memfd export RFC](https://lore.kernel.org/all/20261027154649.91571-1-griffoul@gmail.com/), tree `a6af7e15843c` | [`poc/gmem-bar1-ranges`](https://github.com/jaeyoon-choi/open-gpu-kernel-modules/tree/poc/gmem-bar1-ranges) | [`gpu_gmem_provider`](../gpu_gmem_provider) |

Both kernels also need `IOMMUFD=y`, `VFIO_DEVICE_CDEV=y` and, for the PTE
dumps, `INTEL_IOMMU_DEBUGFS=y`. The `iommu_map_pa` comparison runs on either
kernel with `iommu_map_pa` from this branch.

## Build

```
make pal  KHDR=/path/to/pal-kernel/usr/include
make gmem KHDR=/path/to/gmem-kernel/usr/include
```

Add `CUDA13=1` with CUDA 13 or later. The binaries go to `build/`.

## Tools

| Tool | What it does |
| --- | --- |
| `pal_probe` | Exports a CUDA buffer as a dma-buf and maps it with `IOMMU_IOAS_MAP_FILE`. `--vfio` attaches an NVMe device, `--hold` keeps the mapping for a PTE dump |
| `pal_nvme` | A minimal NVMe driver on a VFIO cdev and iommufd. It writes and reads the GPU buffer through the NVMe device and compares the data with `cuMemcpy` (C1: NVMe writes GPU memory, C2: NVMe reads it), and sends DMA to an unmapped IOVA (D, D2) |
| `bar1_cpu_read` | Reads the CUDA buffer from the CPU through the GPU's BAR1, to check the addresses |

`pal_nvme` options pick the mapping and the measurement:

| Option | Effect |
| --- | --- |
| (none) | Maps the CUDA dma-buf with `IOMMU_IOAS_MAP_FILE` (PAL kernel) |
| `--gmem` | Maps a guest_memfd backed by `gpu_gmem_provider` (gmem build) |
| `--map-pa <BDF>` | Maps with `iommu_map_pa`, like xNVMe (2 MiB pages, IOVA 256 GiB) |
| `--identity`, `--uio <BDF>` | IOVA equal to the BAR1 address, or no translation at all |
| `--gpu-iova A` | Places the GPU mapping at IOVA `A` |
| `--bench rd\|wr`, `--target host` | Sequential I/O throughput into or out of GPU memory, or host memory as the baseline |
| `--map-loop N` | Maps and unmaps the buffer N times and reports the latency |
| `--lifecycle close\|free` | Closes the fds, or calls `cuMemFree`, while the mapping is held, then checks what DMA reaches |

## Scripts

| Script | What it does |
| --- | --- |
| `scripts/followup-run.sh <BDF> <SLBA> [parts]` | Runs the correctness, throughput, map cost, lifecycle, IOVA and PTE tests on one NVMe device. `GPU=gmem` selects the guest_memfd path. Writes LBAs from `SLBA` |
| `scripts/bind-vfio.sh`, `scripts/restore-nvme.sh` | Bind the NVMe device to vfio-pci, and give it back to the nvme driver |
| `scripts/identity-uio.sh` | Puts the NVMe device in an identity domain under uio_pci_generic |
| `scripts/pte-dump.sh` | Prints the device's VT-d page table from debugfs |

The scripts overwrite LBAs on the NVMe device they are given. They refuse
`0000:4d:00.0`, which held a dataset on the test server.
