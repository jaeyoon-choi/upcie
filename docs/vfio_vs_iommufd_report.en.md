# VFIO Implementation vs iommufd Architecture — Comparison Report

> 🌐 [한국어](vfio_vs_iommufd_report.md) · English

This report compares two architectures.

1. The architecture we currently implemented and verified (VFIO-based: udmabuf + `upcie_iommu_map.ko`)
2. The architecture upstream is moving toward (iommufd-based)

Both share the same goal: **direct (P2P) transfer between an NVMe SSD and CUDA GPU memory without going through the CPU/RAM, while keeping IOMMU isolation.**

---

## 0. Background concepts

The IOMMU's address-translation structure mirrors the CPU–MMU relationship. The CPU uses a virtual address (VA) and the MMU translates it to PA; a device uses an IOVA and the IOMMU translates it to PA.

```
   CPU     ──VA───► [ MMU  ] ──► PA
   device  ──IOVA─► [ IOMMU ] ──► PA
```

Terms used in this report:

- **PA (physical address)** — the fixed address where memory physically resides. e.g. GPU memory sits at PA `0x6000000000` and does not change.
- **IOVA** — the address a device uses for DMA (a device-side virtual address). The IOMMU translates it to PA.
- **IOMMU** — hardware that translates the IOVA a device emits into a PA. The IOMMU applies per device, not per memory.
- **domain** — the set of IOVA→PA mappings one device uses. A device is attached to exactly one domain at any instant. An IOVA not present in the mapping is blocked, which is the basis of isolation.

**Principle:** whether translation occurs is decided by the device accessing the memory, not by the memory itself.

- A device that does not go through an IOMMU uses PA directly.
- A device behind an IOMMU uses an IOVA, which the IOMMU translates to PA.

In P2P transfers, translation applies only to the initiator's IOMMU. When the NVMe issues a transfer targeting GPU memory, the NVMe's IOMMU performs the translation; the target side (GPU)'s IOMMU is not involved. Note that this holds only when the P2P TLP is routed up to the root complex (IOMMU). If a switch forwards the P2P directly (turn-around) without ACS redirect, the TLP never reaches the IOMMU, no translation occurs, and IOVA-based access fails.

### 0.1 Relationship between host VA, IOVA, and PA

There are three address layers, but the layout differs depending on whether the buffer is **host DRAM or GPU memory.**

- **host VA** — the address the CPU (user program) uses to access the buffer. The MMU translates it to PA.
- **PA** — the physical address where the buffer resides; the endpoint of translation. (host buffer: DRAM; GPU buffer: GPU BAR/MMIO)
- **IOVA** — the address the device (NVMe) uses for DMA access. The IOMMU translates it to PA.

**(a) host buffer (DRAM)** — the CPU and the device each reach the same DRAM page via the MMU and the IOMMU respectively.

```
   CPU (user program)                  NVMe (device, VFIO)
        |                                    |
        |  host VA                           |  IOVA  (DMA)
        v                                    v
     +-------+                           +--------+
     |  MMU  |                           | IOMMU  |
     +-------+                           +--------+
        |                                    |
        +----------------+   +---------------+
                         v   v
                 +---------------------+
                 |   DRAM page  (PA)   |   = host buffer
                 +---------------------+

   CPU  access: host VA -> MMU   -> PA
   NVMe access: IOVA    -> IOMMU -> PA   (VFIO: device DMA must go through the IOMMU)
```

`VFIO_IOMMU_MAP_DMA(vaddr=host VA, iova=IOVA)` pins the host VA, resolves its PA, and installs `IOVA → PA` into the device domain. That is, it configures the device-side translation so that the device can also reach — via IOVA — the same DRAM page the CPU accesses via host VA.

**(b) GPU buffer (GPU BAR / MMIO)** — the PA is GPU BAR, not DRAM, and there is no pinnable host VA, so **the CPU-side (host VA→MMU) path does not exist.** Only the device-side (IOVA→IOMMU) path remains.

```
   CPU (user program)                  NVMe (device, VFIO)
        |                                    |
        |  CUDA API                          |  IOVA  (DMA)
        |  (no pinnable host VA)             v
        v                                +--------+
   +-----------+                         | IOMMU  |
   | CUDA / GPU|                         +--------+
   +-----------+                             |
        |                                    |
        +----------------+   +---------------+
                         v   v
                 +----------------------------+
                 | GPU BAR/VRAM (PA=0x60...)   |   = GPU buffer (MMIO)
                 +----------------------------+

   CPU  access: via CUDA only  (no host VA -> MMU path; VFIO_IOMMU_MAP_DMA not usable)
   NVMe access: IOVA -> IOMMU -> GPU BAR
```

The CPU handles GPU memory through CUDA, and there is no pinnable host VA corresponding to this PA. Therefore `VFIO_IOMMU_MAP_DMA` (which takes a host VA) cannot be used; instead the PA is obtained through a separate path (udmabuf) and only `IOVA → PA` is installed directly via `iommu_map` (§2). In other words, the GPU buffer has no host VA path; only the device-side IOVA→PA path reaches the GPU BAR through the IOMMU.

The IOVA choices in this implementation are:

| | host buffer | GPU buffer |
|---|---|---|
| host VA | yes (pinnable) | no (GPU memory is not pinnable) |
| PA | hugepage physical | GPU BAR physical (`0x6000000000`) |
| IOVA | equal to PA (identity) | separate from PA, based on `0x4000000000` (256 GB) |
| registration | `VFIO_IOMMU_MAP_DMA` (pin + resolve PA + map in one call) | udmabuf (obtain PA) + `upcie_iommu_map.ko` (`iommu_map`) |

The host buffer sets `map.iova = PA`, making IOVA equal to PA. This keeps existing NVMe code working: even if it writes the PA into a PRP, that value is a valid IOVA. The GPU buffer uses a separate high IOVA range so it does not collide with the host's identity IOVAs.

---

## 1. Problem: no way to register a GPU address once isolation is on

When issuing I/O to the NVMe, the command (PRP) holds the address of the data buffer.

- **UIO (no isolation)** — the PA is written directly. But a DMA to a wrong address can corrupt arbitrary memory; there is no protection.
- **VFIO/iommufd (isolation on)** — an IOVA is written, and that IOVA must be pre-registered in the domain mapping.

The core problem is that there is no standard user API to register GPU memory into this domain mapping.

- The input to `VFIO_IOMMU_MAP_DMA` / `IOAS_MAP` is a user VA. It can map not only a pinnable host RAM VA but also an mmap'd VA pointing at MMIO (`VM_PFNMAP`) — the path VMs use to map peer BARs.
- The problem is that CUDA provides no mmap-able host VA for GPU memory (`mmap()` on a CUDA dma-buf = `-ENOTSUPP`). That is, the API is unusable not because it cannot take MMIO, but because **no VA exists to feed it.**

How this 'GPU address registration' step is handled is the key difference between the two architectures.

---

## 2. Our implementation (VFIO-based)

### 2.1 Components

```
┌──────────────────────── userspace ──────────────────────────┐
│  test program                                                │
│    ├─ host buffer (regular RAM)                              │
│    └─ CUDA GPU buffer                                        │
└──────────────────────────────────────────────────────────────┘
        │ (1) open NVMe          │ (3) extract GPU PA
        ▼                        ▼
┌──────────────┐         ┌──────────────────┐
│  VFIO        │         │  udmabuf module   │  ← Karl's patch
│ /dev/vfio/*  │         │ (import GPU dma-buf│
│  owns NVMe   │         │  → return addr[])  │
└──────────────┘         └──────────────────┘
        │                        │ (4) register phys_lut into the domain
        │                        ▼
        │                ┌──────────────────────┐
        └───────────────►│ upcie_iommu_map.ko    │  ← our module
                         │ (runs iommu_map into   │
                         │  the NVMe's VFIO domain)│
                         └──────────────────────┘
```

The core of the GPU path is two modules: **udmabuf (obtains the GPU PA)** and **`upcie_iommu_map.ko` (the GPU-address registration that the standard API lacks)**. Numbers (1)~(4) correspond to the execution order in §2.2.

### 2.2 Step-by-step flow

```
[1] Open NVMe via VFIO
      -> an empty VFIO domain is attached to the NVMe (initially no mappings)

--- fill the domain (registration) ---------------------------

[2] register host buffer   (standard API)
      VFIO_IOMMU_MAP_DMA(host_va)   -->  domain:  IOVA_h -> host PA

[3] obtain GPU PA
      CUDA -> dma-buf -> udmabuf     -->  phys_lut[] (= GPU BAR PA)

[4] register GPU buffer   (our module)
      upcie_iommu_map.ko: iommu_map  -->  domain:  IOVA_g -> GPU PA

          +------------------------------+
          |  NVMe VFIO domain (result)   |
          |    IOVA_h  ->  host PA        |
          |    IOVA_g  ->  GPU  PA        |
          +------------------------------+

--- execution (DMA) ------------------------------------------

[5] NVMe: emit PRP=IOVA  -->  IOMMU translate  -->  host RAM / GPU BAR
      (host buffer: IOVA_h,   GPU buffer: IOVA_g)
```

[2] is the standard VFIO procedure; [3][4] are the path added for GPU memory. The key point is that [4] does not create a new domain — it uses `iommu_get_domain_for_dev` to fetch the domain the NVMe already uses and registers into it. This makes the GPU buffer translate in the same domain as the host buffer.

**Why udmabuf returns a PA in [3].** udmabuf maps the GPU dma-buf via `dma_buf_attach()` + `dma_buf_map_attachment_unlocked()` and returns the `sg_dma_address()` value to userspace. Whether that value is an IOVA or a PA is determined by the DMA context of the device that performed the attach — and that device is the **udmabuf misc device.**

- The udmabuf misc device has no parent PCI device, so it does not belong to an IOMMU group and uses the direct-DMA (dma-direct) path. Therefore `sg_dma_address()` is not an IOMMU-translated IOVA but the GPU BAR's PA (bus address). In other words, the "physical-ness" of the returned value is not guaranteed by the API; it stems from the fact that the udmabuf device is not subject to IOMMU translation.
- Evidence: the observed phys_lut values matched the GPU BAR1 PA (the `0x6000000000` range), and after `iommu_map(iova → phys_lut[i])` the bidirectional integrity check passed (L40S). Had phys_lut not been the real PA, the P2P transfer in [5] would have landed on a wrong address and failed the check.
- Assumption and verification: the property above depends on the udmabuf device being untranslated. It can be verified by comparing `phys_lut[0]` against the start address of `/sys/bus/pci/devices/<gpu-bdf>/resource1` (the GPU BAR1 PA); on a system where they disagree, the udmabuf device is behind translation and the `iommu_map` target in [4] would be wrong.

### 2.3 Structure summary

```
        userspace                            kernel
   ┌───────────────────┐        ┌───────────────────────────┐
   │ CUDA GPU buffer    │        │  VFIO domain(IOVA→PA map)   │
   │   │ export dma-buf │        │  ┌─────────────────────┐   │
   │   ▼                │        │  │ IOVA_h → host PA     │◄──┼─ ioctl(VFIO_IOMMU_MAP_DMA)
   │ udmabuf ─phys_lut─►├────────┼─►│ IOVA_g → GPU  PA     │◄──┼─ ioctl(UPCIE_IOMMU_MAP)
   │   │                │ ioctl  │  └─────────────────────┘   │   (upcie_iommu_map.ko)
   │   ▼                │        │            ▲               │
   │ write PRP = IOVA   │        │            │ NVMe DMAs with IOVA
   └───────────────────┘        └────────────┼───────────────┘
                                             ▼
                                       reaches host RAM / GPU BAR
```

Note: the figure shows the data flow from the user's perspective. udmabuf itself is a kernel module; the user obtains phys_lut via `/dev/udmabuf` ioctls.

userspace → kernel ioctl calls:

```
[register host buffer]
  ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map)
      map = { vaddr=host_va, iova=IOVA_h, size,
              flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE }

[obtain GPU PA]
  ioctl(udmabuf_fd, UDMABUF_ATTACH, &dmabuf_fd)   // import GPU dma-buf
  ioctl(udmabuf_fd, UDMABUF_GET_MAP, &map)        // -> phys_lut[] (= GPU PA)

[register GPU buffer]
  ioctl(map_fd, UPCIE_IOMMU_MAP, &req)
      req = { bdf, dmabuf_fd, iova_base, page_size, nphys,
              user_phys_ptr = phys_lut,
              prot = UPCIE_IOMMU_MAP_PROT_READ | _WRITE }   // 0 means READ|WRITE
      // dmabuf_fd: the module holds a dma_buf reference so the GPU memory cannot be
      //            freed while mapped (optional; <0 to skip)
      // the kernel runs iommu_map(prot = IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO)
```

Characteristics:

- The PA passes through userspace (udmabuf returns phys_lut to the user).
- Registration (iommu_map) is performed by a dedicated kernel module (because the standard API has no such path).
- Works today; bidirectional integrity was verified on an L40S server.

Safety measures inside the module:

- **Aperture pre-check** — if the requested IOVA range exceeds the domain's address width (aperture), fail early with a clear `-ERANGE` instead of the kernel `iommu_map`'s ambiguous `-EFAULT`.
- **`iommu_iova_to_phys` read-back** — right after installing the mapping, read the page table back and verify it matches the expected PA.
- **Per-fd mapping tracking** — mappings are tracked per fd and torn down automatically on UNMAP or fd close; teardown checks that the domain still exists (in case VFIO was closed first) to avoid use-after-free.
- **dma_buf reference holding** — via `dmabuf_fd`, the module holds a dma_buf reference so the GPU memory cannot be freed while mapped.

---

## 3. iommufd architecture (upstream direction)

### 3.1 Basics: iommufd, IOAS, IOVA_h / IOVA_g

**IOVA_h / IOVA_g notation.** In this report `IOVA_h` is the IOVA assigned to the host buffer and `IOVA_g` is the IOVA assigned to the GPU buffer (subscripts h = host, g = gpu). We choose both values at registration time (see §0.1: host equals PA, GPU uses a separate high range); they only need to not overlap.

**What IOAS is.** An IOAS (I/O Address Space) is the iommufd object for the IOVA address space a device uses for DMA — i.e. the set of IOVA→PA mappings. It refers to the same thing as a VFIO domain, but iommufd exposes it as a userspace object with an id. The actual hardware page table is the HWPT (hardware page table), and the IOAS is the unit that manages mappings on top of it. The user adds mappings to an IOAS via `IOAS_MAP`/`IOAS_MAP_FILE` and attaches devices to the IOAS. (The formal ioctl names are `IOMMU_IOAS_MAP` / `IOMMU_IOAS_MAP_FILE`; this report uses the short forms.)

**Why iommufd is needed separately.** In VFIO type1, IOMMU management is bolted onto the VFIO container, so address-space management is tied to VFIO and hard to extend (address-space sharing, nested translation, PASID, dirty tracking, and — relevant here — dma-buf mapping). iommufd is a standalone subsystem that decouples this IOMMU/DMA address-space management from VFIO and provides it through a single `/dev/iommu`. VFIO can now use iommufd as its IOMMU backend and directly leverage new iommufd features (e.g. `IOAS_MAP_FILE(dma-buf)`). In short, iommufd is not a competitor replacing VFIO but a generic IOMMU-management layer shared by multiple subsystems.

Mapping the interfaces:

```
   VFIO(our impl):  /dev/vfio/<group>  +  VFIO_IOMMU_MAP_DMA
   iommufd       :  /dev/iommu         +  IOAS_MAP / IOAS_MAP_FILE
                    └ IOAS(address space) ── HWPT(mapping=domain) ── device attach
```

### 3.2 Step-by-step flow

```
[1] attach NVMe to an IOAS on iommufd (/dev/iommu)
      -> an empty IOAS(HWPT) is attached to the NVMe (initially no mappings)
      (path: on the VFIO device cdev, VFIO_DEVICE_BIND_IOMMUFD
             -> VFIO_DEVICE_ATTACH_IOMMUFD_PT(ioas_id))

--- fill the IOAS (registration) -----------------------------

[2] register host buffer
      IOAS_MAP(user_va)                -->  IOAS:  IOVA_h -> host PA

[3] register GPU buffer   (the key difference)
      IOAS_MAP_FILE(fd = GPU dma-buf)  -->  IOAS:  IOVA_g -> GPU PA
      * the kernel extracts PA from the dma-buf internally (PAL); the user never sees the PA.

          +------------------------------+
          |  NVMe IOAS / HWPT (result)   |
          |    IOVA_h  ->  host PA        |
          |    IOVA_g  ->  GPU  PA        |
          +------------------------------+

--- execution (DMA) ------------------------------------------

[4] NVMe: emit PRP=IOVA  -->  IOMMU translate  -->  host RAM / GPU BAR
      (host buffer: IOVA_h,   GPU buffer: IOVA_g)
```

The key difference is [3]. For the GPU buffer, passing the dma-buf fd to `IOAS_MAP_FILE` makes the kernel extract the PA internally (PAL) and register it, and userspace never handles the PA. Compared with our implementation in §2, the two steps — GPU-PA 'extraction (udmabuf)' and 'registration (dedicated module)' — are consolidated by the kernel into a single `IOAS_MAP_FILE`.

### 3.3 Structure summary

```
        userspace                            kernel
   ┌───────────────────┐        ┌───────────────────────────┐
   │ CUDA GPU buffer    │        │  IOAS / HWPT(IOVA→PA map)   │
   │   │ export dma-buf │ fd     │  ┌─────────────────────┐   │
   │   └────────────────┼────────┼─►│ IOVA_h → host PA     │◄──┼─ IOAS_MAP
   │  (PA not seen)     │IOAS_   │  │ IOVA_g → GPU  PA     │◄──┼─ IOAS_MAP_FILE
   │                    │MAP_FILE│  └─────────────────────┘   │   (kernel extracts
   │ write PRP = IOVA   │        │            ▲               │    PA via PAL)
   └───────────────────┘        └────────────┼───────────────┘
                                             ▼
                                       reaches host RAM / GPU BAR
```

Characteristics:

- The PA does not pass through userspace (only the dma-buf fd is passed; the kernel extracts the PA internally).
- Registration is performed by iommufd itself (no dedicated module needed).
- Since userspace never handles a raw PA, it is structurally safer for isolation.
- Lifetime management (revoke) is built in: if the GPU memory moves, the kernel invalidates the mapping.

---

## 4. Side-by-side comparison

```
                    │  our impl (VFIO)            │  iommufd
────────────────────┼─────────────────────────────┼──────────────────────────
 user interface     │ /dev/vfio/<group>            │ /dev/iommu (IOAS)
 host mem register  │ VFIO_IOMMU_MAP_DMA(host_va)  │ IOAS_MAP(user_va)
 GPU PA extraction  │ udmabuf → phys_lut (to user) │ kernel-internal PAL (unseen)
 GPU register (core)│ upcie_iommu_map.ko does      │ IOAS_MAP_FILE(dma-buf fd) does
                    │ iommu_map(domain, iova→PA)   │ the same internally
 PA exposed to user │ yes (phys_lut)               │ no (fd only)
 custom kernel mod  │ required (our module)        │ not required (standard API)
 lifetime (revoke)  │ partial (dma_buf ref stops   │ strong (move_notify/revoke)
                    │ free; no move handling)      │
 works today?       │ yes (verified on L40S)       │ partial (§5)
```

The difference between the two architectures is limited to who performs the registration.

```
   [common]  NVMe ─attach→ [domain] ,  PRP=IOVA ,  P2P after IOVA→PA translation

   [differs] who registers the "IOVA_g → GPU PA" entry into the domain mapping

        our impl :  udmabuf(extract PA) + upcie_iommu_map.ko(register)
        iommufd  :  IOAS_MAP_FILE(dma-buf)  — kernel does extraction + registration
```

So the overall structure is identical; the difference is whether the 'register GPU PA into the domain mapping' step is handled by a user-mediated custom module or by a standard kernel API.

---

## 5. iommufd-related upstream status

The upstream work consists of three layers, each a separate patchset submitted by a different author (not a single series).

```
 ① vfio/pci exports device MMIO (BAR) as a PA-based dma-buf (+revoke)
      └ author Leon Romanovsky.  merged (~kernel 6.19). the foundation.
 ② dma-buf gains a mapping type that provides a PA list (PAL) + converts vfio/iommufd
      └ author Jason Gunthorpe (jgg).  in progress, not merged. (not abandoned)
 ③ IOAS_MAP_FILE accepts a dma-buf fd and maps it into an IOAS
      └ iommufd team.  merged (commit 44ebaa1744fd).
        but currently only "dma-buf exported via VFIO-PCI" + "single range" is supported.
```

Patchset / reference links:

- **①** vfio/pci MMIO dma-buf export (Leon Romanovsky) — [LWN 1032302](https://lwn.net/Articles/1032302/), [patchwork v9](https://patchwork.ozlabs.org/project/linux-pci/cover/20251120-dmabuf-vfio-v9-0-d7f71607f371@nvidia.com/), [Phoronix (6.19)](https://www.phoronix.com/news/Linux-6.19-DMA-BUF-VFIO-PCI)
- **②** dma-buf mapping types / PAL (Jason Gunthorpe) — [LWN 1059366](https://lwn.net/Articles/1059366/), [lore v1](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)
- **③** IOAS_MAP_FILE ← dma-buf — commit `44ebaa1744fd`, [doc clarification (298ab7e6, Alex Mastro)](https://lore.kernel.org/all/20260610-tmp-v1-1-b8ccbf557391@fb.com/)

This cannot currently be applied to our target (CUDA GPU). The reasons:

- What ③ supports is a dma-buf exporting the BAR of a VFIO-owned device.
- Our GPU, however, is owned by the CUDA/NVIDIA driver and its dma-buf is exported by CUDA. So it is not a 'VFIO-PCI dma-buf' and is out of scope.
- Binding the GPU to VFIO satisfies the form but makes CUDA unusable, conflicting with the goal of using GPU memory through CUDA.

The reason the currently merged `IOAS_MAP_FILE` works even without PAL is that the dma-buf exported by ① (vfio-pci) is physical-based, so iommufd obtains the single-range physical directly. The generic dma-buf path without PAL yields a `dma_addr_t` (an IOVA when an IOMMU is present), which is an already-translated address and cannot serve as the target (physical) of a new domain mapping. So ① does provide physical, but only for a 'VFIO-owned BAR'; it cannot be applied to a CUDA-owned GPU. Handling a general exporter (CUDA GPU) with multiple ranges requires PAL (②) and the exporter's PAL implementation (⑤). (The physical path of ① itself is only useful as a prototype to validate the iommufd P2P flow without CUDA — §6.4 B2.)

The conditions for switching are one of:

1. The NVIDIA open KMD dma-buf exporter supports the PAL/pin flow.
2. ② (PAL) is generalized to multiple ranges and non-VFIO exporters.

> PAL is not sufficient with importer (iommufd) acceptance alone; the exporter must implement it. Currently even AMDGPU does not support PAL, and NVIDIA is presumed unsupported as well. This exporter support is the biggest bottleneck and a variable external to the project.

Until those conditions are met, our implementation (udmabuf + `upcie_iommu_map.ko`) is a valid self-contained path, in a forward-compatible relationship where the module can be removed and switched over once iommufd matures.

### 5.1 Present comparison: why the module is fast, what iommufd still needs

- **Why upcie_iommu_map.ko is fast.** The new work it requires is narrow and self-contained. On top of the already-used VFIO type1, it obtains the GPU PA with the already-existing udmabuf and adds a thin module (~400 lines) that only performs `iommu_get_domain_for_dev` + `iommu_map`. There is no vendor-driver change and no waiting on upstream merges; it is complete with code we control and is already verified on L40S.
- **Why the iommufd path has a lot to do.** It requires several pieces built by different parties to mature at once: PAL core merge (②, kernel community), the GPU exporter's PAL implementation (⑤, vendor), and multiple-range support (④). On top of that, this project must fully port VFIO type1 → iommufd (⑥). If any one is missing, the CUDA-GPU path does not hold.

In summary, the module is fast now but out-of-tree and has userspace handle a raw PA, so it is weak on isolation and upstreamability. iommufd is standard, safe, and forward-compatible, but its external dependencies make an immediate switch impossible. Therefore we proceed with the module for now and switch as soon as the iommufd conditions are met (§6).

---

## 6. iommufd transition roadmap

Target (end state):

```
user: ioctl(IOAS_MAP_FILE, fd = GPU dma-buf, iova)
  → iommufd extracts the GPU PA internally (PAL) → maps it into the NVMe's IOAS
  → NVMe PRP = IOVA → P2P DMA → GPU BAR
  (no udmabuf, no upcie_iommu_map.ko, user never sees the PA)
```

### 6.1 Completed items (mainline / in progress)

| # | item | author | status |
|---|---|---|---|
| ① | vfio-pci exports device BAR as a PA-based dma-buf (`VFIO_DEVICE_FEATURE_DMA_BUF`) + revoke | Leon Romanovsky | done (~6.19) |
| ③ | `IOAS_MAP_FILE` accepts a dma-buf fd | iommufd team (`44ebaa1744fd`) | done (limited to VFIO-PCI export + single range) |
| — | iommufd invokes the dma-buf pin flow (lifetime/revoke) | Leon (jgg review) | in progress (v7, nearly complete) |

The uAPI (`IOAS_MAP_FILE`+dma-buf) and the foundation (①) are already in place. However, the range that can currently pass is limited to 'a VFIO-owned device BAR, single range'.

### 6.2 Remaining — (A) upstream (external dependency)

| # | item | status | why needed |
|---|---|---|---|
| ② | merge dma-buf PAL mapping type | in progress, not merged | the path where dma-buf formally provides a PA list; prerequisite for multiple ranges and arbitrary exporters |
| ④ | multiple-range support (IOAS_MAP_FILE) | not started (depends on ②) | needed when a GPU buffer is physically fragmented |
| ⑤ | PAL implementation in the GPU exporter (NVIDIA open KMD) | not started (AMD unsupported too) | for the kernel to extract PA from a CUDA dma-buf, the exporter must provide PAL. the biggest gate |

### 6.3 Remaining — (B) this project (self-driven)

| # | item | status | content |
|---|---|---|---|
| ⑥ | port VFIO type1 → iommufd | not started | currently uses `VFIO_TYPE1_IOMMU`. switch to `/dev/iommu`+IOAS+HWPT, bind the device to iommufd, register host buffers via `IOAS_MAP` |
| ⑦ | test the IOAS_MAP_FILE path | not started | dma-buf fd → IOAS map → PRP=IOVA P2P verification; replaces the iommu_map part of the current test |
| ⑧ | remove the module/udmabuf | not started | once ⑤⑥⑦ are done, remove `upcie_iommu_map.ko` and udmabuf |

> Note on ⑥: iommufd has a VFIO type1 compatibility mode (`CONFIG_IOMMUFD_VFIO_CONTAINER`, which emulates the legacy `/dev/vfio/vfio` container API over iommufd), so the existing code may run unmodified. However, new features such as `IOAS_MAP_FILE` require the native iommufd interface (VFIO device cdev + `VFIO_DEVICE_BIND_IOMMUFD`), so ⑥ must port to the native path.

### 6.4 Bridge options (if ⑤ is delayed)

Since ⑤ (NVIDIA PAL) is an external variable, options to work around it or make progress ahead of it:

- **B1. Extend udmabuf into a PAL-capable exporter.** If udmabuf re-exports the imported GPU PA as PAL, then `IOAS_MAP_FILE(udmabuf fd)` becomes viable. This moves the exporter gate from NVIDIA (external) to udmabuf (which we can modify). However, the PA acquisition still relies on the untranslated property, and ② must be merged for the PAL type to exist.
- **B2. End-to-end prototype with a VFIO-owned device BAR.** Instead of the GPU, bind a spare device to vfio-pci, export its BAR, and map it into the NVMe IOAS via `IOAS_MAP_FILE`; this verifies the full P2P flow using only already-merged code (no CUDA needed).
- **B3. Plumbing test of IOAS_MAP_FILE with a memfd/host buffer.** Verify the basics of the iommufd port (⑥) first, without a GPU.

### 6.5 Recommended order

0. **Prerequisite: a test environment with a recent kernel.** This project currently runs kernel 6.8, which contains neither ① (~6.19) nor ③. The B2/B3 prototypes and the ⑥ verification first require a kernel that includes ①③ (roughly 6.19 or later).
1. ⑥ iommufd port (self-driven, essential future infrastructure) — verify plumbing first with B3 (memfd).
2. B2 prototype — verify `IOAS_MAP_FILE` P2P with a VFIO-exported device BAR using merged code only.
3. Track ⑤/② and evaluate B1 — watch PAL merge and NVIDIA exporter progress; contribute to the udmabuf-PAL bridge if needed.
4. ⑦⑧ — once ②⑤ mature, switch the GPU path and remove the module/udmabuf.

Since ⑤ is the biggest bottleneck and an external variable, an end-to-end CUDA-GPU iommufd path is impossible at present. However, ⑥·B2·B3 can start immediately, and completing them puts us in a state ready to switch the moment ⑤ is resolved. Until then, keep the current udmabuf + module configuration.

---

## 7. Summary

```
goal:  direct NVMe ↔ GPU P2P + keep IOMMU isolation

common structure:
  attach the NVMe to a domain,
  register "IOVA → PA" into the domain mapping,
  write IOVA into the PRP, and
  the IOMMU translates it to PA to reach the GPU BAR.

difference (how the GPU PA is registered):
  our impl (VFIO) : udmabuf extracts the PA to userspace,
                    upcie_iommu_map.ko registers it via iommu_map. (works today, L40S-verified)
  iommufd         : pass the GPU dma-buf fd to IOAS_MAP_FILE,
                    the kernel extracts the PA (PAL) and registers it. (standard/safe, CUDA case unsupported)

key points:
  · the IOMMU applies per device, not per memory.
  · whether a PA or an IOVA is used is decided by the accessing device.
  · P2P translation applies only to the initiator (NVMe) domain; the target (GPU) domain is irrelevant.
  · therefore the registration target is the NVMe's domain, into which "IOVA→GPU PA" is registered.
```
