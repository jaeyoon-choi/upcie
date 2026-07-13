# What this patch does — an explainer for newcomers

> 🌐 [한국어](iommu_map_explainer.md) · English

This document explains what the `upcie_iommu_map` kernel module is and why it is needed, building up from the background concepts. It is written so you can read it with no prior knowledge.

---

## 0. What we want to do (the goal)

Our project (`upcie`) is a **storage stack that drives NVMe directly from userspace**. The end goal here is:

> **Move data directly (peer-to-peer) between an NVMe SSD and GPU memory without going through the CPU or system RAM, while keeping VFIO/IOMMU isolation.**

We want two things at the same time:

1. **Direct P2P**: SSD ↔ GPU VRAM data with no detour through CPU/RAM. For example, loading a large dataset / model weights straight from SSD into the GPU → higher bandwidth, lower latency, no CPU·RAM bottleneck. (The same picture NVIDIA GPUDirect Storage targets.)
2. **Keep isolation**: userspace drives the NVMe, but a bad DMA must not be able to corrupt system memory, so the device is **protected by VFIO/IOMMU**. (We do not give up isolation like UIO does.)

The problem is that these two conflict. **Once you turn on isolation (VFIO), there is no standard way to register GPU memory as a DMA target for the NVMe.** This document explains how we broke through that conflict. (§4 below is "why it conflicts", §5–7 are "how we broke through".)

---

## 1. Starting point: NVMe moves data by "address"

When you tell an NVMe SSD to read/write, the CPU does not carry the data byte by byte. Instead you write **"the memory address where the data buffer is"** (a PRP) into the command, and the NVMe **DMAs to that address by itself** (direct memory access) to move the data.

So what matters to the NVMe is **"an address it can reach."** What this "address" actually is — that is the whole story here.

## 2. VFIO: how userspace handles a device "safely"

Normally the NVMe is managed by a kernel driver (`nvme`). But we want a **userspace program to drive the NVMe directly** (a custom storage stack). There are two ways to do this:

- **UIO**: expose the device registers to userspace. Simple, but there is **no isolation (protection).** A DMA to a wrong address can corrupt any memory.
- **VFIO**: provides userspace device control **isolated by the IOMMU**. It allows the device to DMA **only to addresses that were explicitly registered.** Safe.

This is where the **IOMMU** comes in. The IOMMU is the **"address checkpoint"** between the device and memory. It **translates** the address the device emits (an IOVA) into a real PA (physical address), and **blocks any address that was not registered.**

> So with VFIO: you must first register the addresses the device will access with the IOMMU, and if you do not, access is blocked. This "registration" becomes the crux later.

## 3. What we want: direct NVMe ↔ GPU P2P

We want to move data **directly, without going through CPU/system RAM**, between the NVMe and **GPU memory** (peer-to-peer, P2P). e.g. loading SSD data straight into GPU VRAM.

To do that, the PRP in the NVMe command has to hold **an address that points at GPU memory.**

## 4. The wall: a CUDA address cannot just go into VFIO

The problem has two layers.

1. **The address CUDA gives you is a "GPU virtual address".** It is not an address the NVMe or IOMMU understands.
2. VFIO's address-registration API (`VFIO_IOMMU_MAP_DMA`) only accepts **a pinnable host memory address.** GPU memory is not that, so **registration is rejected** (`-EFAULT`). Trying to work around it by `mmap`-ing the GPU dma-buf also fails (`-ENOTSUPP`).

In short, **"there is no standard user API to register GPU memory with the VFIO IOMMU."** That is the wall we have to get over.

Fortunately, the GPU's **actual list of PAs** (`phys_lut`, the GPU BAR1 addresses) can already be obtained through a separate path. We get it via **udmabuf**: when CUDA exports the GPU memory as a dma-buf, udmabuf imports that dma-buf and **returns the per-page PAs to userspace.**

> This "dma-buf import → return PAs" capability was not originally in the kernel's udmabuf; it depends on **Karl's udmabuf patch** (see `xnvme/udmabuf-import`). That patch is **slated to merge upstream.** To summarize: the udmabuf patch handles the "extract the GPU PAs" step (address extraction), and the `upcie_iommu_map` module in this document handles the next step — "register those PAs into the NVMe's VFIO IOMMU domain" (§5–7). The two are not competitors but different stages of one pipeline.

So **we know the addresses (udmabuf). We just have no way to register them (→ this patch solves that).**

## 5. Key concept: the IOMMU domain

One set of the IOMMU's "translation table (address book)" is called a **domain**. The rules:

- **A device is attached to exactly one domain at any instant.**
- When the device issues a DMA, the address is translated by **the translation table of the domain it is attached to at that moment.**

Kinds of domain (only the ones that matter):

| Kind | Translates? | Who manages mappings | When |
|---|---|---|---|
| default (DMA / DMA-FQ) | yes | **kernel, automatically** | when the NVMe uses the `nvme` driver |
| identity (passthrough) | no (1:1) | — | `iommu=pt` (the case where UIO works with raw addresses) |
| **unmanaged** | yes | **the owner (VFIO/user), directly** | **when the NVMe uses `vfio-pci`** |

Key point: when you bind the NVMe to `vfio-pci`, **VFIO moves the NVMe into its own "unmanaged" domain.** That domain is **empty at first** and is meant to be **filled by the owner.**

## 6. Why the "domain" matters

Rewriting our goal precisely:

> **Register the GPU PAs into *the very domain the NVMe actually uses for DMA translation*.**

Which one is "the very domain" decides success or failure:

- **Wrong choice (the old failure)**: register into the kernel's default DMA context → but the NVMe under `vfio-pci` does not use that domain (it was moved to the VFIO domain). Result: when the NVMe DMAs, the translation is wrong and you get a **Data Transfer Error**.
- **Right choice (this patch)**: grab the domain the NVMe is attached to *right now* (= the VFIO unmanaged one) and register into that.

> Note: it is tempting to think "can't I just import the GPU dma-buf with the plain kernel dma-buf API (`dma_buf_attach()` / `dma_buf_map_attachment()`) and put the resulting address in the PRP?" That path is rejected with `-EOPNOTSUPP` under `vfio-pci`. The address `dma_buf_map_attachment()` gives you is relative to the **kernel's default DMA context**, and `vfio-pci` is a `driver_managed_dma` driver, so the kernel's default DMA address space and the address space managed by userspace VFIO are different. In other words, this is the same "wrong domain" failure as above.

So we use the kernel function **`iommu_get_domain_for_dev(nvme)`** to grab "the domain the NVMe uses right now." Creating a new domain would break VFIO's control, and the default domain is not even used, so **grabbing the already-attached domain is the only correct answer.**

## 7. Our patch: a kernel helper does `iommu_map`

The user API has no channel for "register this PA into this domain." So we made a thin kernel module (`upcie_iommu_map`) to open that channel.

What it does (the essence):

```c
domain = iommu_get_domain_for_dev(&nvme->dev);   // the VFIO domain the NVMe uses
for (i = 0; i < nphys; i++)
    iommu_map(domain,
              iova_base + i * page_size,          // an IOVA the user chose
              phys_lut[i],                        // the GPU's real PA
              page_size, IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO);
```

- `iommu_map()` = **"write one line 'IOVA → GPU PA' into that domain's translation table."**
- The unmanaged domain is meant to be "filled by the owner," so filling it directly like this is legitimate. (The user picks `iova_base` so it does not collide with their other mappings.)
- The user puts the returned **IOVA** into the NVMe PRP. The NVMe DMAs to that IOVA → the VFIO domain translates it to the GPU PA → it reaches GPU memory.

That is all. "We already knew the address (= phys_lut); the one missing step was registering it into the domain the NVMe uses" — the kernel does that step on our behalf.

## 8. The full pipeline

```
GPU memory allocation (cudamem)
   │  extract PAs via udmabuf (Karl's patch)
   ▼
phys_lut  = [GPU BAR1 PAs]        (we know the addresses)
   │  ioctl(UPCIE_IOMMU_MAP, phys_lut, iova_base)
   ▼
[upcie_iommu_map kernel module]
   iommu_get_domain_for_dev(NVMe) → VFIO domain
   iommu_map(domain, iova_base+i*ps, phys_lut[i])   (register into the table)
   │  returns: IOVA_BASE
   ▼
user: NVMe command PRP = IOVA_BASE + offset
   ▼
NVMe DMA(IOVA) → IOMMU translates to GPU PA → direct GPU VRAM access ✅
```

## 9. Results and limits

- **storage → GPU (data loading)**: direct P2P **works** while keeping VFIO isolation. Verified on the L40S server with an R-W-R liveness check — reading after flipping the pattern A→B always returns the latest disk content, proving integrity.
- **GPU → storage (saving)**: on the datacenter platform (L40S server), it **works and is integrity-verified**. GPU→disk (P2P read) is repeated over two rounds with distinct patterns, and each round is cross-checked two ways — a host read-back (known-good path) and a disk→GPU (P2P write) round-trip — all `0 mismatch => VERIFIED`. On the consumer platform (i9-14900K) it fails due to a **platform P2P-read limitation** (a hardware trait, not software — the same code fails on the i9 in every version and succeeds on the L40S).

So this patch makes **"VFIO isolation + direct GPU P2P" possible in software**, and how far it actually works depends on the platform's PCIe P2P support. The leading candidate for the variable that decides GPU→storage success across the two platforms is **whether the NVMe and GPU sit under the same PCIe switch** (with a shared switch, the P2P read turns around at the switch instead of going through the root complex). On the i9 the NVMe and GPU hang off different CPU root ports with no shared switch.

## 10. How to run

Build and load the helper module first.

```bash
make -C kernel
sudo rmmod upcie_iommu_map 2>/dev/null
sudo insmod kernel/upcie_iommu_map.ko
ls -l /dev/upcie-iommu-map
```

### VFIO Quick Command

Bind the NVMe to `vfio-pci` and set up hugepages.

```bash
sudo hugepages setup --count 1024
sudo modprobe vfio-pci
sudo devbind --device 0000:02:00.0 --bind vfio-pci
devbind --device 0000:02:00.0 --list
```

With the NVMe bound to `vfio-pci` and the IOMMU group / `/dev/vfio/<group>` present, verify the VFIO success path.

```bash
sudo ./builddir/tests/test_cudamem_iommu_map_nvme_readwrite 0000:02:00.0
```

For the control with no IOMMU translation (raw phys_lut), bind the NVMe to `uio_pci_generic` and run the existing UIO-path test `test_cudamem_nvme_readwrite`.

## 11. Code map

| File | Role |
|---|---|
| `kernel/upcie_iommu_map.c` | kernel helper module (`iommu_get_domain_for_dev` + `iommu_map`) |
| `include/upcie/iommu_map.h` | ioctl UAPI + user wrappers (`upcie_iommu_map_open/add/del/close`) |
| `include/upcie/nvme/nvme_controller_vfio.h` | the VFIO NVMe open path (with gated debug) |
| `tests/test_cudamem_iommu_map_nvme_readwrite.c` | main: VFIO+iommu_map bidirectional P2P verification |
| `tests/test_hostmem_iommu_map_nvme_readwrite.c` | control: host RAM mapped at the same high IOVA |
| `tests/test_cudamem_nvme_readwrite.c` | control (existing): UIO raw phys_lut (no IOMMU translation) |

## One-line summary

> VFIO isolates device DMA into an IOMMU domain. The problem was that VFIO cannot accept GPU PAs; this patch **registers the GPU PAs directly into the very IOMMU domain the NVMe uses, via `iommu_map`**, enabling direct NVMe↔GPU P2P DMA while keeping isolation.
