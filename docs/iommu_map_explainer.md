# 이 패치가 뭘 하는가 — 처음 보는 사람을 위한 설명

> 🌐 한국어 · [English](iommu_map_explainer.en.md)

`upcie_iommu_map` 커널 모듈이 무엇이고 왜 필요한지를, 배경 개념부터 차근차근 쌓아 설명한다. 사전 지식 없이 읽을 수 있게 썼다.

---

## 0. 우리가 하고 싶은 것 (목표)

우리 프로젝트(`upcie`)는 **유저스페이스에서 NVMe를 직접 제어하는 스토리지 스택**이다. 여기서 이루고 싶은 최종 목표는:

> **NVMe SSD와 GPU 메모리 사이에서 데이터를 CPU와 시스템 RAM을 거치지 않고 직접(peer-to-peer) 주고받되, VFIO/IOMMU 격리는 유지한다.**

두 가지를 동시에 원한다:

1. **직접 P2P**: SSD ↔ GPU VRAM 데이터를 CPU/RAM 경유 없이 바로. 예를 들어 대용량 데이터셋/모델 가중치를 SSD에서 GPU로 곧장 로딩 → 대역폭↑, 지연↓, CPU·RAM 병목 제거. (NVIDIA GPUDirect Storage가 노리는 것과 같은 그림)
2. **격리 유지**: NVMe를 유저스페이스가 제어하되, 잘못된 DMA가 시스템 메모리를 망가뜨리지 못하도록 **VFIO/IOMMU로 보호**. (UIO처럼 격리를 포기하지 않는다)

문제는 이 둘이 충돌한다는 점이다. **격리(VFIO)를 켜면 GPU 메모리를 NVMe의 DMA 대상으로 등록할 표준 방법이 없다.** 이 문서는 그 충돌을 어떻게 뚫었는지를 설명한다. (아래 §4가 "왜 충돌하는지", §5~7이 "어떻게 뚫었는지")

---

## 1. 출발점: NVMe는 "주소"로 데이터를 옮긴다

NVMe SSD에 읽기/쓰기를 시킬 때, CPU가 데이터를 일일이 나르지 않는다. 대신 명령(command) 안에 **"데이터 버퍼가 있는 메모리 주소"**(PRP)를 적어주면, NVMe가 **스스로 그 주소로 DMA**(직접 메모리 접근)해서 데이터를 옮긴다.

즉 NVMe에게 중요한 건 **"내가 접근할 수 있는 주소"**다. 이 "주소"가 무엇이냐가 이 글의 전부다.

## 2. VFIO: 유저스페이스가 디바이스를 "안전하게" 다루는 법

보통 NVMe는 커널 드라이버(`nvme`)가 관리한다. 그런데 우리는 **유저스페이스 프로그램이 직접 NVMe를 제어**하고 싶다 (커스텀 스토리지 스택). 이때 두 가지 방법이 있다:

- **UIO**: 디바이스 레지스터를 유저에 노출. 단순하지만 **격리(protection)가 없다.** 잘못된 주소로 DMA하면 아무 메모리나 망가뜨릴 수 있다.
- **VFIO**: 유저스페이스 디바이스 제어를 **IOMMU로 격리**해서 제공. 디바이스가 DMA할 수 있는 주소를 **명시적으로 등록한 것만** 허용한다. 안전하다.

여기서 **IOMMU**가 등장한다. IOMMU는 디바이스와 메모리 사이의 **"주소 검문소"**다. 디바이스가 쏜 주소(IOVA)를 실제 PA(physical address)로 **번역**하고, **등록 안 된 주소는 막는다.**

> 그래서 VFIO를 쓰면: 디바이스가 접근할 주소를 먼저 IOMMU에 등록해야 하고, 등록 안 하면 접근이 차단된다. 이 "등록"이 뒤에서 핵심이 된다.

## 3. 하고 싶은 것: NVMe ↔ GPU 직접 P2P

우리는 NVMe와 **GPU 메모리** 사이에 데이터를 **CPU/시스템 RAM을 거치지 않고 직접** 옮기고 싶다 (peer-to-peer, P2P). 예: SSD의 데이터를 GPU VRAM으로 바로 로딩.

그러려면 NVMe 명령의 PRP에 **GPU 메모리를 가리키는 주소**를 넣어야 한다.

## 4. 벽: CUDA 주소는 VFIO에 그냥 못 넣는다

문제가 두 겹이다.

1. **CUDA가 주는 주소는 "GPU 가상주소"**다. NVMe나 IOMMU가 이해하는 주소가 아니다.
2. VFIO의 주소 등록 API(`VFIO_IOMMU_MAP_DMA`)는 **"핀(pin) 가능한 호스트 메모리 주소"만** 받는다. GPU 메모리는 그런 게 아니라서 **등록이 거부된다**(`-EFAULT`). GPU dma-buf를 `mmap`해서 우회하려 해도 실패(`-ENOTSUPP`).

즉 **"GPU 메모리를 VFIO IOMMU에 등록할 표준 유저 API가 없다."** 이게 우리가 넘어야 할 벽이다.

다행히 GPU의 **실제 PA 목록**(`phys_lut`, GPU BAR1 주소들)은 별도 경로로 이미 알아낼 수 있다. 우리는 **udmabuf**를 통해 이를 얻는다: CUDA가 GPU 메모리를 dma-buf로 export하면, udmabuf가 그 dma-buf를 import해서 **페이지별 PA를 유저스페이스로 돌려준다.**

> 이 "dma-buf import → PA 반환" 기능은 원래 커널 udmabuf에 없던 것으로, **Karl이 추가한 udmabuf 패치**(참고: `xnvme/udmabuf-import`)에 의존한다. 이 패치는 **업스트림 커널 머지 예정**이다. 정리하면 udmabuf 패치는 "GPU PA를 꺼내주는" 단계(주소 추출)를 담당하고, 이 문서의 `upcie_iommu_map` 모듈은 그 다음 단계 — "그 PA를 NVMe의 VFIO IOMMU domain에 등록"(§5~7) — 를 담당한다. 둘은 경쟁이 아니라 파이프라인의 서로 다른 단계다.

즉 **주소는 안다(udmabuf). 등록할 방법이 없을 뿐이다(→ 이 패치가 해결).**

## 5. 열쇠 개념: IOMMU domain

IOMMU의 "번역표(주소록)" 한 벌을 **domain**이라 한다. 규칙:

- **디바이스 하나는 어느 순간이든 정확히 domain 하나에만** 붙어 있다.
- 디바이스가 DMA를 쏘면, **그 순간 붙어있는 domain의 번역표**로 주소가 번역된다.

domain 종류(중요한 것만):

| 종류 | 번역 | 매핑 관리 | 언제 |
|---|---|---|---|
| 기본(DMA / DMA-FQ) | 함 | **커널이 자동 관리** | NVMe가 `nvme` 드라이버일 때 |
| identity(passthrough) | 안 함(1:1) | — | `iommu=pt` (UIO가 raw 주소로 되는 경우) |
| **unmanaged** | 함 | **소유자(VFIO/유저)가 직접** | **NVMe가 `vfio-pci`일 때** |

핵심: NVMe를 `vfio-pci`에 붙이면, **VFIO가 NVMe를 자기 소유의 "unmanaged" domain으로 옮긴다.** 이 domain은 처음엔 **텅 비어** 있고, **소유자가 채우는** 용도다.

## 6. 왜 "domain"을 따져야 하나

우리 목표를 정확히 다시 쓰면:

> **GPU PA를, NVMe가 *실제로 DMA 번역에 쓰는 바로 그 domain*에 등록한다.**

"바로 그 domain"이 어느 것이냐가 성패를 가른다:

- **틀린 선택 (예전 실패)**: 커널 기본 DMA 문맥에 등록 → 그런데 `vfio-pci` 하의 NVMe는 그 domain을 안 쓴다(VFIO domain으로 옮겨감). 결과: NVMe가 DMA하면 번역이 엉뚱해져 **Data Transfer Error**.
- **옳은 선택 (이 패치)**: NVMe가 *지금* 붙어있는 domain(=VFIO unmanaged)을 그대로 집어서 거기에 등록.

> 참고: "그냥 커널 dma-buf API(`dma_buf_attach()` / `dma_buf_map_attachment()`)로 GPU dma-buf를 import해서 나온 주소를 PRP에 넣으면 되지 않나?"라고 생각하기 쉽다. 이 경로는 `vfio-pci`에서 `-EOPNOTSUPP`로 거부된다. `dma_buf_map_attachment()`가 주는 주소는 **커널 기본 DMA 문맥** 기준이고, `vfio-pci`는 `driver_managed_dma` 드라이버라 커널 기본 DMA 주소공간과 유저 VFIO가 관리하는 주소공간이 다르기 때문이다. 즉 이것도 위의 "틀린 domain"과 같은 실패다.

그래서 커널 함수 **`iommu_get_domain_for_dev(nvme)`**로 "NVMe가 지금 쓰는 domain"을 집어온다. 새 domain을 만들면 VFIO 제어가 깨지고, 기본 domain은 쓰이지도 않으니, **이미 붙어있는 그 domain을 가져오는 게 유일한 정답**이다.

## 7. 우리 패치: 커널 helper가 `iommu_map` 한다

유저 API엔 "이 domain에 이 PA를 등록하라"는 통로가 없다. 그래서 얇은 커널 모듈(`upcie_iommu_map`)을 만들어 그 통로를 뚫었다.

동작(핵심만):

```c
domain = iommu_get_domain_for_dev(&nvme->dev);   // NVMe가 쓰는 VFIO domain
for (i = 0; i < nphys; i++)
    iommu_map(domain,
              iova_base + i * page_size,          // 유저가 고른 IOVA
              phys_lut[i],                        // GPU 실제 PA
              page_size, IOMMU_READ | IOMMU_WRITE | IOMMU_MMIO);
```

- `iommu_map()` = **"그 domain 번역표에 'IOVA → GPU PA' 한 줄을 쓴다."**
- unmanaged domain은 "소유자가 채우는" 용도라 이렇게 직접 채우는 게 정당하다. (유저는 `iova_base`를 자기 다른 매핑과 안 겹치게 고른다.)
- 유저는 반환된 **IOVA**를 NVMe PRP에 넣는다. NVMe가 그 IOVA로 DMA → VFIO domain이 GPU PA로 번역 → GPU 메모리 도달.

이게 전부다. "주소는 알고 있었고(=phys_lut), 그걸 NVMe가 쓰는 domain에 등록하는 한 단계"가 빠져 있었는데, 그 단계를 커널에서 대신 해준 것.

## 8. 전체 파이프라인

```
GPU 메모리 할당 (cudamem)
   │  udmabuf(Karl 패치)로 PA 추출
   ▼
phys_lut  = [GPU BAR1 PA들]           (주소는 안다)
   │  ioctl(UPCIE_IOMMU_MAP, phys_lut, iova_base)
   ▼
[upcie_iommu_map 커널 모듈]
   iommu_get_domain_for_dev(NVMe) → VFIO domain
   iommu_map(domain, iova_base+i*ps, phys_lut[i])   (번역표에 등록)
   │  반환: IOVA_BASE
   ▼
유저: NVMe 명령 PRP = IOVA_BASE + offset
   ▼
NVMe DMA(IOVA) → IOMMU가 GPU PA로 번역 → GPU VRAM 직접 접근 ✅
```

## 9. 결과와 한계

- **스토리지 → GPU (데이터 로딩)**: VFIO 격리를 유지한 채 직접 P2P **동작**. L40S 서버에서 패턴 A→B를 바꿔가며 read해도 항상 최신 디스크 내용을 받는 R-W-R liveness로 무결성 검증.
- **GPU → 스토리지 (저장)**: 데이터센터 플랫폼(L40S 서버)에선 **동작 + 무결성 검증**. GPU→disk(P2P read)를 서로 다른 패턴으로 2라운드 반복하고, 각 라운드를 host read-back(known-good 경로)과 disk→GPU(P2P write) 왕복 두 방법으로 대조해 모두 `0 mismatch => VERIFIED`. 소비자 플랫폼(i9-14900K)에선 **플랫폼의 P2P read 한계**로 실패 (소프트웨어가 아니라 하드웨어 특성 — 같은 코드가 i9에선 어떤 버전이든 실패, L40S에선 성공).

즉 이 패치는 **"VFIO 격리 + GPU 직접 P2P"를 소프트웨어적으로 가능하게** 하고, 실제 되는 범위는 플랫폼의 PCIe P2P 지원에 달려 있다. 두 플랫폼의 GPU→스토리지 성패를 가르는 핵심 변수로는 **NVMe와 GPU가 같은 PCIe 스위치 아래에 있는지**(공통 스위치가 있으면 P2P read가 root complex를 안 거치고 스위치에서 turn-around)가 유력하다. i9는 NVMe·GPU가 서로 다른 CPU root port에 매달려 있고 공유 스위치가 없다.

## 10. 실행 방법

helper 모듈을 먼저 빌드·로드한다.

```bash
make -C kernel
sudo rmmod upcie_iommu_map 2>/dev/null
sudo insmod kernel/upcie_iommu_map.ko
ls -l /dev/upcie-iommu-map
```

### VFIO Quick Command

NVMe를 `vfio-pci`에 bind하고 hugepage를 준비한다.

```bash
sudo hugepages setup --count 1024
sudo modprobe vfio-pci
sudo devbind --device 0000:02:00.0 --bind vfio-pci
devbind --device 0000:02:00.0 --list
```

NVMe가 `vfio-pci`에 bind되어 있고 IOMMU group과 `/dev/vfio/<group>`이 존재하는 상태에서 VFIO 성공 경로를 확인한다.

```bash
sudo ./builddir/tests/test_cudamem_iommu_map_nvme_readwrite 0000:02:00.0
```

IOMMU 번역이 없는 대조군(raw phys_lut)은 NVMe를 `uio_pci_generic`에 bind한 뒤 기존 UIO 경로 테스트 `test_cudamem_nvme_readwrite`로 실행한다.

## 11. 코드 지도

| 파일 | 역할 |
|---|---|
| `kernel/upcie_iommu_map.c` | 커널 helper 모듈 (`iommu_get_domain_for_dev` + `iommu_map`) |
| `include/upcie/iommu_map.h` | ioctl UAPI + 유저 wrapper (`upcie_iommu_map_open/add/del/close`) |
| `include/upcie/nvme/nvme_controller_vfio.h` | VFIO로 NVMe 여는 경로 (게이팅 디버그 포함) |
| `tests/test_cudamem_iommu_map_nvme_readwrite.c` | 메인: VFIO+iommu_map 양방향 P2P 검증 |
| `tests/test_hostmem_iommu_map_nvme_readwrite.c` | 대조: host RAM을 같은 high-IOVA에 매핑 |
| `tests/test_cudamem_nvme_readwrite.c` | 대조(기존): UIO raw phys_lut (IOMMU 번역 없음) |

## 한 줄 요약

> VFIO는 디바이스 DMA를 IOMMU domain으로 격리한다. GPU PA를 VFIO가 못 받아주는 게 문제였고, 이 패치는 **NVMe가 실제 쓰는 그 IOMMU domain에 GPU PA를 `iommu_map`으로 직접 등록**해서, 격리를 유지한 채 NVMe↔GPU 직접 P2P DMA를 가능하게 한다.
