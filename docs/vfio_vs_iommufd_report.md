# VFIO 구현과 iommufd 구조 비교 보고서

> 🌐 한국어 · [English](vfio_vs_iommufd_report.en.md)

본 보고서는 다음 두 구조를 비교한다.

1. 현재 구현하여 동작을 확인한 구조 (VFIO 기반: udmabuf + `upcie_iommu_map.ko`)
2. upstream이 지향하는 구조 (iommufd 기반)

두 구조의 목표는 동일하다. **NVMe SSD와 CUDA GPU 메모리 사이에서 CPU/RAM을 경유하지 않는 직접(P2P) 전송을 수행하되, IOMMU 격리를 유지하는 것이다.**

---

## 0. 배경 개념

IOMMU의 주소 변환 구조는 CPU–MMU 관계와 동일하다. CPU는 가상주소(VA)를 사용하고 MMU가 이를 PA로 변환하며, 디바이스는 IOVA를 사용하고 IOMMU가 이를 PA로 변환한다.

```
   CPU     ──VA───► [ MMU  ] ──► PA
   디바이스 ──IOVA─► [ IOMMU ] ──► PA
```

본 보고서에서 사용하는 용어는 다음과 같다.

- **PA (physical address)** — 메모리가 실제 위치하는 고정 주소. 예: GPU 메모리는 PA `0x6000000000`에 위치하며 변하지 않는다.
- **IOVA** — 디바이스가 DMA에 사용하는 주소(디바이스용 가상주소). IOMMU가 이를 PA로 변환한다.
- **IOMMU** — 디바이스가 발행한 IOVA를 PA로 변환하는 하드웨어. IOMMU는 메모리가 아니라 디바이스 단위로 적용된다.
- **domain** — 한 디바이스가 사용하는 IOVA→PA 매핑의 집합. 디바이스는 임의의 시점에 하나의 domain에만 연결된다. 매핑에 없는 IOVA는 차단되며, 이것이 격리의 근거가 된다.

**원칙:** 주소 변환 여부는 메모리가 아니라 해당 메모리에 접근하는 디바이스가 결정한다.

- IOMMU를 거치지 않는 디바이스의 접근은 PA를 그대로 사용한다.
- IOMMU 뒤의 디바이스는 IOVA를 사용하며, IOMMU가 이를 PA로 변환한다.

P2P 전송에서 변환은 요청을 발행하는 측(initiator)의 IOMMU에만 적용된다. NVMe가 GPU 메모리를 대상으로 전송을 발행하면 NVMe의 IOMMU가 변환을 수행하며, 대상 측(GPU)의 IOMMU는 관여하지 않는다. 단, 이는 P2P TLP가 root complex(IOMMU)까지 라우팅된다는 전제에서 성립한다. 스위치가 ACS redirect 없이 P2P를 직접 전달(turn-around)하면 TLP가 IOMMU에 도달하지 않아 변환 자체가 일어나지 않으며, 이 경우 IOVA 기반 접근은 실패한다.

### 0.1 host VA · IOVA · PA 관계

주소 계층은 다음 세 가지다. 다만 버퍼가 **host DRAM인지 GPU 메모리인지에 따라 구성이 달라진다.**

- **host VA** — CPU(유저 프로그램)가 버퍼에 접근할 때 사용하는 주소. MMU가 PA로 변환한다.
- **PA** — 버퍼가 실제 위치하는 물리 주소. 변환의 종착점이다. (host 버퍼는 DRAM, GPU 버퍼는 GPU BAR/MMIO)
- **IOVA** — 디바이스(NVMe)가 DMA로 접근할 때 사용하는 주소. IOMMU가 PA로 변환한다.

**(a) host 버퍼 (DRAM)** — CPU와 디바이스 두 경로가 각각 MMU/IOMMU를 통해 같은 DRAM 페이지에 도달한다.

```
   CPU (유저 프로그램)                 NVMe (디바이스, VFIO)
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
                 |   DRAM page  (PA)   |   = host 버퍼
                 +---------------------+

   CPU  접근: host VA -> MMU   -> PA
   NVMe 접근: IOVA    -> IOMMU -> PA   (VFIO: 디바이스 DMA는 반드시 IOMMU 경유)
```

`VFIO_IOMMU_MAP_DMA(vaddr=host VA, iova=IOVA)`는 host VA를 pin하여 그 PA를 확인한 뒤 디바이스 domain에 `IOVA → PA`를 설치한다. 즉 CPU가 host VA로 접근하는 DRAM 페이지에 디바이스가 IOVA로도 도달하도록 디바이스 측 변환을 구성한다.

**(b) GPU 버퍼 (GPU BAR / MMIO)** — PA가 DRAM이 아니라 GPU BAR이며, pin 가능한 host VA가 없어 **CPU 측(host VA→MMU) 경로가 존재하지 않는다.** 디바이스 측(IOVA→IOMMU) 경로만 있다.

```
   CPU (유저 프로그램)                 NVMe (디바이스, VFIO)
        |                                    |
        |  CUDA API                          |  IOVA  (DMA)
        |  (pin 가능한 host VA 없음)         v
        v                                +--------+
   +-----------+                         | IOMMU  |
   | CUDA / GPU|                         +--------+
   +-----------+                             |
        |                                    |
        +----------------+   +---------------+
                         v   v
                 +----------------------------+
                 | GPU BAR/VRAM (PA=0x60...)   |   = GPU 버퍼 (MMIO)
                 +----------------------------+

   CPU  접근: CUDA 통해서만  (host VA -> MMU 경로 없음, VFIO_IOMMU_MAP_DMA 불가)
   NVMe 접근: IOVA -> IOMMU -> GPU BAR
```

CPU는 GPU 메모리를 CUDA를 통해 다루며, 이 PA에 대응하는 pin 가능한 host VA가 없다. 그래서 `VFIO_IOMMU_MAP_DMA`(host VA 입력)를 쓸 수 없고, PA를 별도 경로(udmabuf)로 확보한 뒤 `iommu_map`으로 `IOVA → PA`만 직접 설치한다(§2). 즉 GPU 버퍼에는 host VA 경로가 없고, 디바이스 측 IOVA→PA 경로만 IOMMU를 통해 GPU BAR에 도달한다.

본 구현의 IOVA 선택은 다음과 같다.

| | host 버퍼 | GPU 버퍼 |
|---|---|---|
| host VA | 있음 (pin 가능) | 없음 (GPU 메모리는 pin 불가) |
| PA | hugepage 물리 | GPU BAR 물리 (`0x6000000000`) |
| IOVA | PA와 동일 (identity) | PA와 별개, `0x4000000000`(256GB) 기반 |
| 등록 방법 | `VFIO_IOMMU_MAP_DMA` (pin+PA해석+map 일괄) | udmabuf(PA 확보) + `upcie_iommu_map.ko`(`iommu_map`) |

host 버퍼는 `map.iova = PA`로 설정하여 IOVA와 PA를 일치시킨다. 이는 기존 NVMe 코드가 PRP에 PA를 기입하더라도 그 값이 유효한 IOVA가 되도록 하기 위함이다. GPU 버퍼는 host의 identity IOVA와 충돌하지 않도록 별도의 고대역 IOVA를 사용한다.

---

## 1. 문제 정의: 격리 활성화 시 GPU 주소 등록 수단의 부재

NVMe에 I/O를 지시할 때 명령(PRP)에는 데이터 버퍼의 주소를 기입한다.

- **UIO(격리 없음)** — PA를 직접 기입한다. 다만 잘못된 주소로의 DMA가 임의 메모리를 훼손할 수 있어 보호 수단이 없다.
- **VFIO/iommufd(격리 있음)** — IOVA를 기입하며, 해당 IOVA가 domain 매핑에 사전 등록되어 있어야 한다.

핵심 문제는 GPU 메모리를 이 domain 매핑에 등록할 표준 유저 API가 존재하지 않는다는 점이다.

- `VFIO_IOMMU_MAP_DMA` / `IOAS_MAP`의 입력은 유저 VA다. pin 가능한 host RAM VA뿐 아니라 MMIO를 가리키는 mmap된 VA(`VM_PFNMAP`)도 매핑할 수 있다 (VM이 peer BAR를 매핑할 때 쓰는 경로).
- 문제는 CUDA가 GPU 메모리에 대해 mmap 가능한 host VA를 제공하지 않는다는 점이다(CUDA dma-buf `mmap()` = `-ENOTSUPP`). 즉 API가 MMIO를 못 받아서가 아니라, **입력으로 넣을 VA 자체가 존재하지 않아** 이 API를 사용할 수 없다.

이 'GPU 주소 등록' 단계를 처리하는 방식이 두 구조의 핵심 차이다.

---

## 2. 본 구현 (VFIO 기반)

### 2.1 구성 요소

```
┌──────────────────────── 유저스페이스 ────────────────────────┐
│  테스트 프로그램                                              │
│    ├─ host 버퍼(일반 RAM)                                    │
│    └─ CUDA GPU 버퍼                                          │
└──────────────────────────────────────────────────────────────┘
        │ (1) NVMe 열기          │ (3) GPU PA 추출
        ▼                        ▼
┌──────────────┐         ┌──────────────────┐
│  VFIO        │         │  udmabuf 커널모듈  │  ← Karl 패치
│ /dev/vfio/*  │         │ (GPU dma-buf import│
│  NVMe 소유   │         │  → 주소 배열 반환) │
└──────────────┘         └──────────────────┘
        │                        │ (4) phys_lut 를 domain 에 등록
        │                        ▼
        │                ┌──────────────────────┐
        └───────────────►│ upcie_iommu_map.ko    │  ← 본 구현 모듈
                         │ (NVMe의 VFIO domain에  │
                         │  iommu_map 직접 실행)  │
                         └──────────────────────┘
```

GPU 경로의 핵심은 **udmabuf(GPU PA 확보)** 와 **`upcie_iommu_map.ko`(표준 API에 없는 GPU 주소 등록)** 두 모듈이다. 그림의 번호 (1)~(4)는 §2.2의 실행 순서에 대응한다.

### 2.2 단계별 흐름

```
[1] NVMe 를 VFIO 로 열기
      -> NVMe 에 빈 VFIO domain 이 연결됨 (초기: 매핑 없음)

--- domain 채우기 (등록) -------------------------------------

[2] host 버퍼 등록   (표준 API)
      VFIO_IOMMU_MAP_DMA(host_va)   -->  domain:  IOVA_h -> host PA

[3] GPU PA 확보
      CUDA -> dma-buf -> udmabuf     -->  phys_lut[] (= GPU BAR PA)

[4] GPU 버퍼 등록   (전용 모듈)
      upcie_iommu_map.ko: iommu_map  -->  domain:  IOVA_g -> GPU PA

          +------------------------------+
          |  NVMe VFIO domain (result)   |
          |    IOVA_h  ->  host PA        |
          |    IOVA_g  ->  GPU  PA        |
          +------------------------------+

--- 실행 (DMA) -----------------------------------------------

[5] NVMe: PRP=IOVA 발행  -->  IOMMU 변환  -->  host RAM / GPU BAR
      (host 버퍼: IOVA_h,   GPU 버퍼: IOVA_g)
```

[2]는 표준 VFIO 절차이고 [3][4]가 GPU를 위해 추가된 경로다. 핵심은 [4]에서 새 domain을 생성하지 않고 `iommu_get_domain_for_dev`로 NVMe가 이미 사용 중인 domain을 가져와 등록한다는 점이다. 이를 통해 GPU 버퍼가 host 버퍼와 동일한 domain에서 함께 변환된다.

**[3]에서 udmabuf가 PA를 반환하는 근거.** udmabuf는 GPU dma-buf를 `dma_buf_attach()` + `dma_buf_map_attachment_unlocked()`로 매핑한 뒤 `sg_dma_address()` 값을 유저에 반환한다. 이 값이 IOVA인지 PA인지는 attach를 수행한 디바이스의 DMA 문맥이 결정하며, 그 주체는 **udmabuf misc 디바이스**다.

- udmabuf misc 디바이스는 부모 PCI 디바이스가 없어 IOMMU group에 속하지 않으며, direct DMA(dma-direct) 경로를 사용한다. 따라서 `sg_dma_address()`는 IOMMU 변환을 거친 IOVA가 아니라 GPU BAR의 PA(버스 주소)가 된다. 즉 반환값의 '물리성'은 API가 보장하는 성질이 아니라, udmabuf 디바이스가 IOMMU 변환 대상이 아니라는 사실에서 비롯된다.
- 실증: 관측된 phys_lut 값이 GPU BAR1 PA(`0x6000000000` 대역)와 일치했고, `iommu_map(iova → phys_lut[i])` 등록 후 양방향 무결성 검증(L40S)을 통과했다. phys_lut이 실제 PA가 아니었다면 [5]의 P2P 전송이 잘못된 주소에 도달하여 검증에 실패한다.
- 전제 및 검증: 위 성질은 udmabuf 디바이스가 untranslated라는 조건에 의존한다. `phys_lut[0]`를 `/sys/bus/pci/devices/<gpu-bdf>/resource1`의 시작 주소(GPU BAR1 PA)와 대조하여 확인할 수 있으며, 값이 어긋나는 시스템에서는 udmabuf 디바이스가 변환 뒤에 있다는 의미이므로 [4]의 `iommu_map` 대상이 틀어진다.

### 2.3 구조 요약

```
        유저스페이스                         커널
   ┌───────────────────┐        ┌───────────────────────────┐
   │ CUDA GPU 버퍼      │        │  VFIO domain(IOVA→PA 매핑)  │
   │   │ export dma-buf │        │  ┌─────────────────────┐   │
   │   ▼                │        │  │ IOVA_h → host PA     │◄──┼─ ioctl(VFIO_IOMMU_MAP_DMA)
   │ udmabuf ─phys_lut─►├────────┼─►│ IOVA_g → GPU  PA     │◄──┼─ ioctl(UPCIE_IOMMU_MAP)
   │   │                │ ioctl  │  └─────────────────────┘   │   (upcie_iommu_map.ko)
   │   ▼                │        │            ▲               │
   │ PRP = IOVA 기록    │        │            │ NVMe 가 IOVA 로 DMA
   └───────────────────┘        └────────────┼───────────────┘
                                             ▼
                                       host RAM / GPU BAR 도달
```

※ 그림은 유저 관점의 데이터 흐름이다. udmabuf 자체는 커널 모듈이며, 유저는 `/dev/udmabuf` ioctl로 phys_lut을 받는다.

유저스페이스 → 커널 ioctl 호출:

```
[host 버퍼 등록]
  ioctl(container_fd, VFIO_IOMMU_MAP_DMA, &map)
      map = { vaddr=host_va, iova=IOVA_h, size,
              flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE }

[GPU PA 추출]
  ioctl(udmabuf_fd, UDMABUF_ATTACH, &dmabuf_fd)   // GPU dma-buf import
  ioctl(udmabuf_fd, UDMABUF_GET_MAP, &map)        // -> phys_lut[] (= GPU PA)

[GPU 버퍼 등록]
  ioctl(map_fd, UPCIE_IOMMU_MAP, &req)
      req = { bdf, dmabuf_fd, iova_base, page_size, nphys,
              user_phys_ptr = phys_lut,
              prot = UPCIE_IOMMU_MAP_PROT_READ | _WRITE }   // 0이면 READ|WRITE
      // dmabuf_fd: 모듈이 dma_buf 참조를 잡아 매핑 기간 GPU 메모리 해제 방지(선택, <0이면 생략)
      // 커널이 iommu_map(prot = IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO) 수행
```

특징:

- PA가 유저스페이스를 경유한다(udmabuf가 phys_lut을 유저에 반환).
- 등록(iommu_map)은 전용 커널 모듈이 수행한다(표준 API에 해당 경로가 없기 때문).
- 현재 동작하며, L40S 서버에서 양방향 무결성 검증을 완료했다.

모듈 내부 안전장치:

- **aperture 사전검사** — 요청 IOVA 범위가 domain의 주소폭(aperture)을 넘으면, 커널 `iommu_map`의 모호한 `-EFAULT` 대신 명확한 `-ERANGE`로 조기 실패시킨다.
- **`iommu_iova_to_phys` 역검증** — 매핑 설치 직후 페이지테이블을 되읽어 기대 PA와 일치하는지 확인한다.
- **per-fd 매핑 추적** — 매핑을 fd 단위로 추적하여 UNMAP 또는 fd close 시 자동 해제하며, 해제 시 domain 생존을 확인해(VFIO가 먼저 닫힌 경우) use-after-free를 방지한다.
- **dma_buf 참조 유지** — `dmabuf_fd`로 dma_buf 참조를 잡아 매핑 기간 동안 GPU 메모리 해제를 방지한다.

---

## 3. iommufd 구조 (upstream 방향)

### 3.1 기본 개념: iommufd, IOAS, IOVA_h / IOVA_g

**IOVA_h / IOVA_g 표기.** 본 보고서에서 `IOVA_h`는 host 버퍼에 할당한 IOVA, `IOVA_g`는 GPU 버퍼에 할당한 IOVA를 가리킨다(아래첨자 h = host, g = gpu). 두 값 모두 등록 시 우리가 선택하며(§0.1 참고: host는 PA와 동일하게, GPU는 별도 고대역), 서로 겹치지 않도록만 하면 된다.

**IOAS(I/O Address Space)란.** 한 디바이스가 DMA에 사용하는 IOVA 주소공간, 즉 IOVA→PA 매핑의 집합을 iommufd에서 다루는 객체다. VFIO의 domain과 동일한 대상을 가리키지만, iommufd에서는 이를 id를 가진 유저스페이스 객체로 노출한다. 실제 하드웨어 페이지테이블은 HWPT(hardware page table)가 담당하고, IOAS는 그 위에서 매핑을 관리하는 단위다. 유저는 IOAS에 `IOAS_MAP`/`IOAS_MAP_FILE`로 매핑을 추가하고, 디바이스를 IOAS에 attach한다. (정식 ioctl 명칭은 `IOMMU_IOAS_MAP` / `IOMMU_IOAS_MAP_FILE`이며, 본 보고서는 축약형으로 표기한다.)

**왜 iommufd가 별도로 필요한가.** VFIO type1은 IOMMU 관리 기능이 VFIO 컨테이너에 붙어 있어 주소공간 관리가 VFIO에 종속되고 확장이 어렵다(주소공간 공유, 중첩 변환, PASID, dirty tracking, 그리고 여기서 필요한 dma-buf 매핑 등). iommufd는 이 IOMMU/DMA 주소공간 관리를 VFIO에서 분리하여 `/dev/iommu` 하나로 제공하는 독립 서브시스템이다. VFIO는 이제 iommufd를 IOMMU 백엔드로 사용할 수 있으며, iommufd가 제공하는 새 기능(예: `IOAS_MAP_FILE(dma-buf)`)을 그대로 활용한다. 즉 iommufd는 VFIO의 대체 경쟁자가 아니라, 여러 서브시스템이 공유하는 범용 IOMMU 관리 계층이다.

인터페이스를 대응시키면 다음과 같다.

```
   VFIO(본 구현):  /dev/vfio/<group>  +  VFIO_IOMMU_MAP_DMA
   iommufd     :  /dev/iommu         +  IOAS_MAP / IOAS_MAP_FILE
                  └ IOAS(주소공간) ── HWPT(매핑=domain) ── 디바이스 attach
```

### 3.2 단계별 흐름

```
[1] NVMe 를 iommufd(/dev/iommu) 의 IOAS 에 attach
      -> NVMe 에 빈 IOAS(HWPT) 가 연결됨 (초기: 매핑 없음)
      (경로: VFIO device cdev 에서 VFIO_DEVICE_BIND_IOMMUFD
             -> VFIO_DEVICE_ATTACH_IOMMUFD_PT(ioas_id))

--- IOAS 채우기 (등록) ---------------------------------------

[2] host 버퍼 등록
      IOAS_MAP(user_va)                -->  IOAS:  IOVA_h -> host PA

[3] GPU 버퍼 등록   (핵심 차이)
      IOAS_MAP_FILE(fd = GPU dma-buf)  -->  IOAS:  IOVA_g -> GPU PA
      * 커널이 dma-buf 에서 PA 를 내부 추출(PAL). 유저는 PA 미인지.

          +------------------------------+
          |  NVMe IOAS / HWPT (result)   |
          |    IOVA_h  ->  host PA        |
          |    IOVA_g  ->  GPU  PA        |
          +------------------------------+

--- 실행 (DMA) -----------------------------------------------

[4] NVMe: PRP=IOVA 발행  -->  IOMMU 변환  -->  host RAM / GPU BAR
      (host 버퍼: IOVA_h,   GPU 버퍼: IOVA_g)
```

핵심 차이는 [3]이다. GPU 버퍼는 dma-buf fd를 `IOAS_MAP_FILE`로 전달하면 커널이 내부에서 PA를 추출(PAL)하여 등록하며, 유저스페이스는 PA를 다루지 않는다. §2의 본 구현과 비교하면, GPU PA의 '추출(udmabuf)'과 '등록(전용 모듈)' 두 단계를 커널이 `IOAS_MAP_FILE` 하나로 통합한 것이 차이다.

### 3.3 구조 요약

```
        유저스페이스                         커널
   ┌───────────────────┐        ┌───────────────────────────┐
   │ CUDA GPU 버퍼      │        │  IOAS / HWPT(IOVA→PA 매핑)  │
   │   │ export dma-buf │ fd     │  ┌─────────────────────┐   │
   │   └────────────────┼────────┼─►│ IOVA_h → host PA     │◄──┼─ IOAS_MAP
   │  (PA 미인지)       │IOAS_   │  │ IOVA_g → GPU  PA     │◄──┼─ IOAS_MAP_FILE
   │                    │MAP_FILE│  └─────────────────────┘   │   (커널이 PAL로
   │ PRP = IOVA 기록    │        │            ▲               │    PA 추출)
   └───────────────────┘        └────────────┼───────────────┘
                                             ▼
                                       host RAM / GPU BAR 도달
```

특징:

- PA가 유저스페이스를 경유하지 않는다(dma-buf fd만 전달하며 커널 내부에서 PA를 추출).
- 등록을 iommufd가 직접 수행한다(전용 모듈이 불필요).
- 유저스페이스가 raw PA를 다루지 않으므로 격리 측면에서 구조적으로 안전하다.
- 수명관리(revoke)가 내장되어, GPU 메모리가 이동하면 커널이 매핑을 무효화한다.

---

## 4. 구조 비교

```
                    │  본 구현 (VFIO)             │  iommufd
────────────────────┼─────────────────────────────┼──────────────────────────
 유저 인터페이스     │ /dev/vfio/<group>            │ /dev/iommu (IOAS)
 host 메모리 등록    │ VFIO_IOMMU_MAP_DMA(host_va)  │ IOAS_MAP(user_va)
 GPU PA 추출         │ udmabuf → phys_lut (유저로)  │ 커널 내부 PAL (유저 미인지)
 GPU 등록(핵심)      │ upcie_iommu_map.ko 가        │ IOAS_MAP_FILE(dma-buf fd) 가
                    │ iommu_map(domain, iova→PA)   │ 내부에서 동일 작업 수행
 PA 유저 노출        │ 있음 (phys_lut)              │ 없음 (fd만)
 커스텀 커널모듈     │ 필요 (전용 모듈)             │ 불필요 (표준 API)
 수명관리(revoke)    │ 부분(dma_buf 참조로 해제만    │ 강함(move_notify/revoke)
                    │ 방지, 이동 대응 없음)        │
 현재 동작 여부      │ 동작 (L40S 검증 완료)        │ 부분 지원 (5장)
```

두 구조의 차이는 등록 주체에 국한된다.

```
   [공통]  NVMe ─attach→ [domain] ,  PRP=IOVA ,  IOVA→PA 변환 후 P2P

   [차이]  domain 매핑에 "IOVA_g → GPU PA" 항목을 등록하는 주체

        본 구현   :  udmabuf(PA 추출) + upcie_iommu_map.ko(등록)
        iommufd  :  IOAS_MAP_FILE(dma-buf)  — 추출과 등록을 커널이 일괄 수행
```

즉 전체 구조는 동일하며, 'GPU PA를 domain 매핑에 등록하는 단계'를 유저 경유 커스텀 모듈로 처리하는지, 커널 표준 API로 처리하는지의 차이다.

---

## 5. iommufd 관련 upstream 현황

upstream 작업은 세 계층으로 구성되며, 각 계층은 서로 다른 저자가 제출한 별개의 패치셋이다(단일 시리즈가 아니다).

```
 ① vfio/pci 가 디바이스 MMIO(BAR)를 PA 기반 dma-buf 로 export (+revoke)
      └ 저자 Leon Romanovsky.  merged (~커널 6.19). 토대.
 ② dma-buf 가 PA 목록(PAL)을 제공하는 mapping type 추가 + vfio/iommufd 전환
      └ 저자 Jason Gunthorpe(jgg).  진행 중, 미병합. (중단된 것은 아님)
 ③ IOAS_MAP_FILE 가 dma-buf fd 를 받아 IOAS 에 매핑
      └ iommufd 팀.  merged (커밋 44ebaa1744fd).
        단, 현재는 "VFIO-PCI로 export된 dma-buf" + "단일 range" 만 지원.
```

패치셋/자료 링크:

- **①** vfio/pci MMIO dma-buf export (Leon Romanovsky) — [LWN 1032302](https://lwn.net/Articles/1032302/), [patchwork v9](https://patchwork.ozlabs.org/project/linux-pci/cover/20251120-dmabuf-vfio-v9-0-d7f71607f371@nvidia.com/), [Phoronix(6.19)](https://www.phoronix.com/news/Linux-6.19-DMA-BUF-VFIO-PCI)
- **②** dma-buf mapping types / PAL (Jason Gunthorpe) — [LWN 1059366](https://lwn.net/Articles/1059366/), [lore v1](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)
- **③** IOAS_MAP_FILE ← dma-buf — 커밋 `44ebaa1744fd`, [주석 정정(298ab7e6, Alex Mastro)](https://lore.kernel.org/all/20260610-tmp-v1-1-b8ccbf557391@fb.com/)

본 구현 대상(CUDA GPU)에는 현재 적용할 수 없다. 근거는 다음과 같다.

- ③이 지원하는 것은 VFIO가 소유한 디바이스의 BAR를 export한 dma-buf다.
- 반면 본 구현의 GPU는 CUDA/NVIDIA 드라이버가 소유하며 dma-buf도 CUDA가 export한다. 따라서 'VFIO-PCI dma-buf'가 아니어서 지원 대상에 포함되지 않는다.
- GPU를 VFIO에 바인딩하면 형식은 충족하나 CUDA를 사용할 수 없게 되어, CUDA로 GPU 메모리를 사용한다는 목적과 충돌한다.

현재 merge된 `IOAS_MAP_FILE`이 PAL 없이도 동작하는 이유는, ①(vfio-pci)이 export하는 dma-buf가 physical 기반이어서 iommufd가 단일 range의 physical을 직접 얻기 때문이다. PAL이 없는 일반 dma-buf 경로는 `dma_addr_t`(IOMMU가 있으면 IOVA)를 제공하며, 이는 이미 번역된 주소이므로 새 도메인 매핑의 타깃(physical)으로 쓸 수 없다. 즉 ①은 physical을 제공하지만 'VFIO가 소유한 BAR' 전용이므로 CUDA가 소유한 GPU에는 적용할 수 없고, 일반 exporter(CUDA GPU)를 다중 range로 다루려면 PAL(②)과 exporter의 PAL 구현(⑤)이 필요하다. (①의 physical 경로 자체는 CUDA 없이 iommufd P2P 흐름을 검증하는 프로토타입 용도로만 유효하다 — §6.4 B2)

전환 가능 조건은 다음 중 하나다.

1. NVIDIA open KMD의 dma-buf exporter가 PAL/pin 흐름을 지원한다.
2. ②(PAL)가 다중 range 및 비-VFIO exporter까지 일반화된다.

> PAL은 importer(iommufd)의 수용만으로는 충분하지 않으며 exporter의 구현이 필요하다. 현재 AMDGPU도 PAL을 지원하지 않으며 NVIDIA 역시 미지원으로 판단된다. 이 exporter 지원이 최대 병목이자 프로젝트 외부 변수다.

해당 조건이 충족되기 전까지는 본 구현(udmabuf + `upcie_iommu_map.ko`)이 유효한 자립 경로이며, iommufd가 성숙한 시점에 모듈을 제거하고 전환할 수 있는 상위호환 관계다.

### 5.1 현시점 비교: 모듈이 빠른 이유, iommufd의 잔여 작업

- **upcie_iommu_map.ko가 빠른 이유.** 이 방식이 요구하는 신규 작업은 좁고 자체 완결적이다. 이미 사용 중인 VFIO type1 위에, 이미 존재하는 udmabuf로 GPU PA를 확보하고, `iommu_get_domain_for_dev` + `iommu_map`만 수행하는 얇은 모듈(약 400줄)을 추가하면 된다. 벤더 드라이버 변경이나 upstream 병합 대기가 없고, 우리가 통제하는 코드만으로 완결되며 이미 L40S에서 동작을 확인했다.
- **iommufd 경로에 할 일이 많은 이유.** 이 방식은 서로 다른 주체가 만드는 여러 조각이 동시에 성숙해야 한다: PAL 코어 병합(②, 커널 커뮤니티), GPU exporter의 PAL 구현(⑤, 벤더), 다중 range 지원(④). 여기에 본 프로젝트의 VFIO type1 → iommufd 전면 포팅(⑥)이 더해진다. 어느 하나라도 빠지면 CUDA-GPU 경로가 성립하지 않는다.

정리하면, 모듈은 지금 빠르지만 out-of-tree이며 유저가 raw PA를 다룬다는 점에서 격리·업스트림성이 약하다. iommufd는 표준·안전·상위호환이지만 외부 의존이 커서 즉시 전환이 불가능하다. 따라서 당분간 모듈로 진행하고, iommufd 조건이 갖춰지는 대로 전환한다(§6).

---

## 6. iommufd 전환 로드맵

목표(end state):

```
유저: ioctl(IOAS_MAP_FILE, fd = GPU dma-buf, iova)
  → iommufd 가 커널 내부에서 GPU PA 추출(PAL) → NVMe 의 IOAS 에 매핑
  → NVMe PRP = IOVA → P2P DMA → GPU BAR
  (udmabuf 불필요, upcie_iommu_map.ko 불필요, 유저 PA 미인지)
```

### 6.1 완료 항목 (mainline / 진행)

| # | 항목 | 저자 | 상태 |
|---|---|---|---|
| ① | vfio-pci 가 디바이스 BAR 를 PA 기반 dma-buf 로 export (`VFIO_DEVICE_FEATURE_DMA_BUF`) + revoke | Leon Romanovsky | 완료 (~6.19) |
| ③ | `IOAS_MAP_FILE` 이 dma-buf fd 수용 | iommufd 팀 (`44ebaa1744fd`) | 완료 (단, VFIO-PCI export + 단일 range 한정) |
| — | iommufd 가 dma-buf pin 흐름 호출 (수명/revoke) | Leon (jgg 리뷰) | 진행 중 (v7, 거의 완성) |

uAPI(`IOAS_MAP_FILE`+dma-buf)와 토대(①)는 이미 확보되었다. 다만 현재 통과 가능한 범위는 'VFIO 소유 디바이스 BAR, 단일 range'에 한정된다.

### 6.2 잔여 항목 — (A) upstream (외부 의존)

| # | 항목 | 상태 | 필요 사유 |
|---|---|---|---|
| ② | dma-buf PAL mapping type 병합 | 진행 중, 미병합 | dma-buf가 PA 목록을 정식으로 제공하는 경로. 다중 range·임의 exporter의 전제 |
| ④ | 다중 range 지원 (IOAS_MAP_FILE) | 미착수 (②에 의존) | GPU 버퍼가 물리적으로 여러 조각인 경우 필요 |
| ⑤ | GPU exporter(NVIDIA open KMD)의 PAL 구현 | 미착수 (AMD도 미지원) | CUDA dma-buf에서 커널이 PA를 추출하려면 exporter의 PAL 제공이 필요. 최대 관문 |

### 6.3 잔여 항목 — (B) 본 프로젝트 (자체 수행)

| # | 항목 | 상태 | 내용 |
|---|---|---|---|
| ⑥ | VFIO type1 → iommufd 포팅 | 미착수 | 현재 `VFIO_TYPE1_IOMMU` 사용. `/dev/iommu`+IOAS+HWPT, 디바이스 iommufd 바인딩, host 버퍼는 `IOAS_MAP`으로 전환 |
| ⑦ | IOAS_MAP_FILE 경로 테스트 | 미착수 | dma-buf fd → IOAS 매핑 → PRP=IOVA P2P 검증. 현재 테스트의 iommu_map 부분을 대체 |
| ⑧ | 모듈/udmabuf 제거 | 미착수 | ⑤⑥⑦ 완료 시 `upcie_iommu_map.ko` 및 udmabuf 제거 |

> 참고(⑥): iommufd에는 VFIO type1 호환 모드(`CONFIG_IOMMUFD_VFIO_CONTAINER`, 기존 `/dev/vfio/vfio` 컨테이너 API를 iommufd가 에뮬레이션)가 있어 기존 코드가 무수정 동작할 수 있다. 그러나 `IOAS_MAP_FILE` 등 신기능은 native iommufd 인터페이스(VFIO device cdev + `VFIO_DEVICE_BIND_IOMMUFD`)가 필요하므로, ⑥은 native 경로로 포팅해야 한다.

### 6.4 브릿지 옵션 (⑤ 지연 시)

⑤(NVIDIA PAL)가 외부 변수이므로, 우회 또는 선행 진행을 위한 대안은 다음과 같다.

- **B1. udmabuf를 PAL-capable exporter로 확장.** udmabuf가 import한 GPU PA를 PAL로 재-export하면 `IOAS_MAP_FILE(udmabuf fd)`가 성립한다. exporter 관문을 NVIDIA(외부)에서 udmabuf(자체 수정 가능)로 이전한다. 단 PA 획득이 여전히 untranslated 특성에 의존하며, ②가 병합되어야 PAL 타입이 존재한다.
- **B2. VFIO 소유 디바이스 BAR로 end-to-end 프로토타입.** GPU 대신 여분 디바이스를 vfio-pci에 바인딩하여 BAR를 export하고 `IOAS_MAP_FILE`로 NVMe IOAS에 매핑하면, 이미 병합된 코드만으로 P2P 전체 흐름을 검증할 수 있다(CUDA 불요).
- **B3. memfd/host 버퍼로 IOAS_MAP_FILE 배관 테스트.** GPU 없이 iommufd 포팅(⑥)의 기본 동작을 우선 검증한다.

### 6.5 권장 순서

0. **선행 조건: 최신 커널 테스트 환경 확보.** 본 프로젝트 환경은 커널 6.8이며, ①(~6.19)·③이 포함되어 있지 않다. B2/B3 프로토타입과 ⑥ 검증에는 ①③이 포함된 커널(약 6.19 이상) 환경이 먼저 필요하다.
1. ⑥ iommufd 포팅 (자체 수행, 향후 필수 인프라) — B3(memfd)로 배관부터 검증.
2. B2 프로토타입 — VFIO-export 디바이스 BAR로 `IOAS_MAP_FILE` P2P를 병합 코드만으로 검증.
3. ⑤/② 추적 및 B1 검토 — PAL 병합과 NVIDIA exporter 동향을 주시하고, 필요 시 udmabuf-PAL 브릿지에 기여.
4. ⑦⑧ — ②⑤ 성숙 시 GPU 경로 전환 및 모듈/udmabuf 제거.

⑤가 최대 병목이자 외부 변수이므로 현시점에서 CUDA-GPU end-to-end iommufd는 불가능하다. 다만 ⑥·B2·B3는 즉시 착수 가능하며, 이를 완료하면 ⑤ 해소 시점에 즉시 전환할 수 있는 상태가 된다. 그 이전까지는 현행 udmabuf + 모듈 구성을 유지한다.

---

## 7. 요약

```
목표:  NVMe ↔ GPU 직접 P2P + IOMMU 격리 유지

공통 구조:
  NVMe 를 domain 에 연결하고,
  domain 매핑에 "IOVA → PA" 를 등록하고,
  PRP 에 IOVA 를 기입하면,
  IOMMU 가 PA 로 변환하여 GPU BAR 에 도달한다.

차이 (GPU PA 를 등록하는 방법):
  본 구현(VFIO) : udmabuf 로 PA 를 유저에 추출하고,
                  upcie_iommu_map.ko 가 iommu_map 으로 등록. (현재 동작, L40S 검증)
  iommufd       : GPU dma-buf fd 를 IOAS_MAP_FILE 로 전달하면,
                  커널이 내부에서 PA 추출(PAL) 후 등록. (표준·안전, CUDA 케이스는 미지원)

핵심 정리:
  · IOMMU 는 메모리가 아니라 디바이스 단위로 적용된다.
  · PA 를 사용하는지 IOVA 를 사용하는지는 접근하는 디바이스가 결정한다.
  · P2P 변환은 발행 측(initiator=NVMe)의 domain 에만 적용되며, 대상 측(GPU) domain 은 무관하다.
  · 따라서 등록 대상은 NVMe 의 domain 이며, 여기에 "IOVA→GPU PA" 를 등록한다.
```
