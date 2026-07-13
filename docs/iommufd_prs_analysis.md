# upcie iommufd PR(#37~#39) 분석 보고서

Simon(safl)이 upstream upcie 저장소에 올린 세 개의 PR을 분석한다. 세 PR은 하나의 묶음으로, **iommufd 기반의 새 DMA 경로를 upcie에 도입**하는 작업이다.

| PR | 제목 | 역할 |
|---|---|---|
| #37 | feat: an umbrella for iommu-guarded I/O, dma-buf exporters, NVMe importer | 기반 공사 (dmamem + iommufd) |
| #38 | feat(dmamem): dma-buf constructor + NVMe DMA into GPU VRAM | 핵심 PoC (NVMe→GPU VRAM) |
| #39 | refactor(nvme): unify vfio-pci path + share bring-up helpers | 경로 통합 리팩터 |

관련 배경(왜 iommufd인가, 본 구현과의 구조 비교)은 `docs/vfio_vs_iommufd_report.md`를 참고한다. 이하 PA = physical address.

---

## 1. 한눈에 보는 결론

- 세 PR은 본 구현의 비교 보고서 §6 로드맵 중 **자체 수행 항목(⑥ iommufd 포팅, B2 VFIO BAR PoC, B3 memfd 배관 검증)을 그대로 구현**한 것이다.
- #38은 mainline 커널(6.19+)만으로 NVMe→GPU VRAM 직접 DMA를 증명했다. 단, **GPU를 vfio-pci에 바인딩해야 하므로 그 GPU에서 CUDA는 사용 불가**하다 (4장).
- 따라서 **"CUDA가 소유한 GPU 메모리"를 NVMe DMA 대상으로 만드는 경로는 여전히 본 구현(udmabuf + `upcie_iommu_map.ko`)뿐**이다. 두 작업은 경쟁이 아니라 상보 관계다 (5장).

---

## 2. PR #37 — dmamem: iommufd 기반 공사

### 무엇을 하나

기존 upcie의 메모리 계층은 `hostmem`(hugepage 확보 후 페이지별 PA를 phys_lut로 추적)이다. #37은 그 **iommufd 버전 형제**인 `dmamem`을 신설한다.

핵심 아이디어는 phys_lut의 제거다:

```
기존 hostmem:  버퍼가 물리적으로 조각남
               -> phys_lut[i] 로 페이지별 PA 추적, PRP 생성 시마다 조회

dmamem:        커널이 IOAS 에 "연속된 IOVA 창"을 할당
               -> iova = base_iova + offset  으로 끝. 물리 배치를 볼 필요 없음
```

IOMMU가 어차피 IOVA→PA를 변환하므로, 물리가 조각나 있어도 IOVA는 연속으로 만들 수 있다는 점을 이용한 것이다. 디바이스에 PA 배치가 노출되던 UIO 시절 설계에서 벗어난다.

### iommufd 초기화 순서 (비교 보고서 §3.2 [1]의 native 경로와 동일)

```
open(/dev/iommu)
  -> IOMMU_IOAS_ALLOC                      (IOAS 생성)
open(/dev/vfio/devices/vfioN)              (vfio cdev)
  -> VFIO_DEVICE_BIND_IOMMUFD              (디바이스를 iommufd 에 바인딩)
  -> VFIO_DEVICE_ATTACH_IOMMUFD_PT(ioas)   (IOAS 에 attach)
```

### 메모리 확보와 매핑

```
memfd_create(MFD_HUGETLB)  ->  mlock  ->  IOMMU_IOAS_MAP_FILE(memfd fd)
                                           -> 커널이 IOVA 를 골라서 반환
```

- IOVA를 유저가 고르지 않고 **커널이 할당**해 돌려받는다 (본 구현은 유저가 고정 base를 선택 — 충돌 관리가 유저 몫이었던 것과 대비).
- `IOMMU_IOAS_MAP_FILE`이 없는 6.14 미만 커널에서는 `IOAS_MAP(host VA)`로 폴백한다.

### 신규 파일

`include/upcie/iommufd.h`(ioctl 래퍼), `dmamem.h`(공유 타입), `dmamem_memfd.h`(memfd 생성자), `dmamem_heap.h`(오프셋 서브할당자), `nvme/nvme_controller_dmamem.h`(cdev 경로 NVMe open) + IDENTIFY smoketest.

설계상 **생성자(constructor)를 갈아끼우는 구조**라서, memfd 외에 dma-buf·VFIO BAR·(미래에) GPU 생성자를 제출 코드 수정 없이 추가할 수 있다. #38이 그 첫 확장이다.

---

## 3. PR #38 — NVMe가 GPU VRAM에 직접 DMA (mainline만으로)

### 무엇을 증명했나

커스텀 커널 코드 없이, **mainline 6.19+ 커널만으로** NVMe가 GPU VRAM에 직접 DMA함을 보였다. 흐름:

```
1. GPU 를 vfio-pci 에 바인딩                          <- 4장의 핵심 제약 지점
2. GPU 의 BAR 조각을 dma-buf 로 export
     ioctl(gpu_vfio_fd, VFIO_DEVICE_FEATURE,
           { GET | DMA_BUF, region_index, {offset, length} })
     -> dma-buf fd                                   (= Leon Romanovsky 의 ①)
3. 그 fd 를 NVMe 가 붙은 IOAS 에 import
     IOMMU_IOAS_MAP_FILE(fd, READABLE|WRITEABLE)
     -> base_iova 반환                               (= ③)
4. NVMe 명령의 PRP 에 그 IOVA 기입 -> NVMe 가 VRAM 에 직접 DMA
```

### 어떻게 확인했나

- IDENTIFY(0x06) 결과를 VRAM IOVA(offset 0x2000)에 착지시키고, CPU가 GPU BAR을 mmap해 시리얼 넘버를 읽어 확인.
- IO qpair 생성 후 NVMe READ(0x02, LBA 0, 8블록)를 VRAM offset 0x4000으로 → BAR mmap으로 디스크 데이터 착지 확인.

### 한계

- **CUDA 미사용** — GPU가 vfio-pci 소유라 CUDA 자체가 불능 (4장 상술). 검증도 GPU 연산이 아니라 CPU의 BAR mmap이다.
- **storage→GPU 방향만 시연** — GPU→storage(NVMe가 GPU를 read하는 P2P read) 검증은 없다.
- **커널 6.19+ 필요** — 본 프로젝트 환경(6.8)에서는 실행 불가.

---

## 4. "GPU를 vfio-pci에 바인딩" = CUDA 포기의 의미

이 PR에서 가장 중요한 설계 선택이며, PR 본문·커밋에 **명시적으로 쓰여 있지 않아** 오독하기 쉽다.

### 전제: 디바이스는 드라이버 하나에만 바인딩된다

```
GPU ── nvidia.ko (NVIDIA KMD)   <- CUDA 가 동작하는 세계
GPU ── vfio-pci                 <- #38 이 요구하는 세계
     (동시에 둘 다는 불가)
```

CUDA 스택 전체(`cuInit`, `cudaMalloc`, 커널 실행, `/dev/nvidia*`)는 nvidia.ko가 GPU에 붙어 있어야만 동작한다. GPU를 vfio-pci로 옮기는 순간 그 GPU에서는 어떤 CUDA 연산도 불가능하다.

### 왜 반드시 vfio-pci여야 하나

BAR를 dma-buf로 export하는 ioctl(`VFIO_DEVICE_FEATURE_DMA_BUF`)이 **vfio fd에만 존재**하기 때문이다. mainline에서 `IOMMU_IOAS_MAP_FILE`이 받아주는 유일한 dma-buf exporter가 vfio-pci다(#38 커밋 메시지도 "the one exporter iommufd_ioas_map_file accepts on mainline 6.19+"라고 명시). nvidia.ko에는 이 export 기능이 없다. 즉 이 바인딩은 테스트 편의가 아니라 **이 방식의 구조적 전제**다.

바인딩이 일어나는 위치는 C 코드가 아니라 테스트 오케스트레이션이다(`cijoe/tasks/dmamem_nvme_vram_smoketest.yaml`의 `bind_gpu_group` 스텝). GPU의 IOMMU group 전체를 sysfs `unbind` → `driver_override=vfio-pci` → `bind`로 넘긴다. 이 `unbind` 한 줄이 nvidia.ko에서 GPU가 떨어져 나가는 지점이다.

### 결과적으로 #38의 "GPU VRAM"이란

- CUDA가 할당한 버퍼가 아니라, **ReBAR로 노출된 raw VRAM의 임의 offset**이다.
- 할당자도 없고, 그 데이터를 소비할 GPU 프로그램도 없다.
- GPU는 "연산 가속기"가 아니라 **"큰 메모리 창(BAR)이 달린 수동적 PCIe 디바이스"** 역할만 한다.

따라서 #38은 **배관(iommufd + dma-buf + IOAS_MAP_FILE) 증명으로서는 완결적**이지만, "SSD 데이터를 CUDA 버퍼로 로딩해 GPU가 연산한다"는 GPUDirect Storage류 워크플로는 아니다.

### Simon도 이를 인지하고 있다

커밋 메시지에 "CUDA/HIP/libdrm exports for GPU VRAM"은 mainline이 비(非)vfio-pci exporter를 열어주면 "thin dmamem_from_* wrapper"로 추가될 미래 작업이라고 적혀 있다. 즉 인프라 선행 → CUDA exporter는 upstream(PAL 등)을 기다린다는 전략이며, 이는 비교 보고서 §6의 ②⑤ 분석과 일치한다.

---

## 5. PR #39 — type1/iommufd 경로 통합

#37로 NVMe open 경로가 2개(type1 컨테이너, iommufd cdev)가 되자 중복을 정리한 리팩터다.

- 드라이버: `NVME_BACKEND_VFIO`/`_DMAMEM` → `NVME_BACKEND_VFIO_TYPE1`/`_VFIO_IOMMUFD`. 환경변수 `UPCIE_VFIO_MODE=auto|type1|iommufd`로 런타임 선택. auto는 `/dev/iommu` 존재 여부로 iommufd 우선, 없으면 type1 폴백.
- 헤더: 공통 bring-up 4종(`bus_master_enable`, `acquire_bar0`, `reset_via_bar0`, `enable_via_bar0`)을 신규 `nvme_controller_vfio_pci.h`로 추출. 두 경로의 차이는 fd 획득(type1 group vs iommufd cdev)과 매핑 API(`VFIO_IOMMU_MAP_DMA` vs `IOMMU_IOAS_MAP_FILE`)만 남는다.
- 규모: 7파일, +175/−198.

**본 구현과의 충돌**: 본 브랜치(`vfio_cudamem_final`)가 수정한 `nvme_controller_vfio.h`의 bring-up 영역을 이 PR이 리팩터하므로, 머지 시 rebase가 필요하다.

---

## 6. 본 구현과의 관계

### 6.0 (복습) 비교 보고서 §6의 iommufd 전환 로드맵

목표(end state): `ioctl(IOAS_MAP_FILE, fd = GPU dma-buf, iova)` 한 번으로 커널이 GPU PA를 내부 추출(PAL)하여 NVMe의 IOAS에 매핑 — udmabuf 불필요, `upcie_iommu_map.ko` 불필요, 유저는 PA를 만지지 않음.

**완료 (upstream mainline):**

| # | 항목 | 상태 |
|---|---|---|
| ① | vfio-pci가 디바이스 BAR를 PA 기반 dma-buf로 export (`VFIO_DEVICE_FEATURE_DMA_BUF`) — Leon Romanovsky | 완료 (~6.19) |
| ③ | `IOAS_MAP_FILE`이 dma-buf fd 수용 | 완료 (단, **VFIO-PCI export + 단일 range 한정**) |

**잔여 (upstream, 외부 의존):**

| # | 항목 | 상태 |
|---|---|---|
| ② | dma-buf **PAL**(PA 목록) mapping type 병합 — jgg | 진행 중, 미병합 |
| ④ | 다중 range 지원 | 미착수 (②에 의존) |
| ⑤ | **GPU exporter(NVIDIA open KMD)의 PAL 구현** | 미착수. **최대 관문**, 우리 통제 밖 |

**잔여 (본 프로젝트, 자체 수행):**

| # | 항목 | 내용 |
|---|---|---|
| ⑥ | VFIO type1 → iommufd 포팅 | `/dev/iommu` + IOAS + vfio cdev(native) 경로로 전환 |
| ⑦ | IOAS_MAP_FILE 경로 테스트 | dma-buf fd → IOAS 매핑 → PRP=IOVA P2P 검증 |
| ⑧ | 모듈/udmabuf 제거 | ⑤⑥⑦ 완료 시 |

**브릿지 옵션 (⑤ 지연 대비):** B1 = udmabuf를 PAL-capable exporter로 확장 / B2 = VFIO 소유 디바이스 BAR로 end-to-end 프로토타입(CUDA 불요) / B3 = memfd로 IOAS_MAP_FILE 배관 검증.

**선행조건 0:** ①③이 포함된 커널(≈6.19+) 환경 확보 (본 프로젝트 환경은 6.8).

### 6.1 로드맵 대응 — 비교 보고서 §6이 실행된 것

| 비교 보고서 §6 항목 | 이번 PR |
|---|---|
| ⑥ type1→iommufd 포팅 (native cdev) | #37 + #39 로 구현 |
| B3 memfd 로 IOAS_MAP_FILE 배관 검증 | #37 smoketest 로 구현 |
| B2 VFIO BAR export 로 end-to-end PoC | #38 로 구현 |
| 선행조건 0 (6.19+ 커널) | #38 이 요구사항으로 명시 |
| **⑤ CUDA GPU (NVIDIA exporter)** | **여전히 공백** |
| ② PAL / ④ 다중 range | upstream 미병합 (변동 없음) |

### 6.2 상보 관계

```
Simon (#37~39):  NVMe -> vfio-pci,  GPU -> vfio-pci (CUDA 불능)
                 mainline 배관 증명. GPU 는 수동 DMA 타깃.

본 구현:         NVMe -> vfio-pci,  GPU -> nvidia.ko (CUDA 유지)
                 CUDA 버퍼로 실제 로딩. GPU 주소는 udmabuf 로 추출,
                 upcie_iommu_map.ko 가 NVMe domain 에 등록. (L40S 검증)
```

겹치는 부분이 없고, 합치면 완성형이 된다. CUDA 케이스를 메우는 선택지는:

1. **하이브리드**: dmamem에 CUDA constructor 추가 — NVMe는 dmamem/iommufd로 열고, CUDA 버퍼만 udmabuf + `upcie_iommu_map.ko`로 같은 domain에 등록. (`iommu_get_domain_for_dev`는 type1이든 iommufd HWPT든 attach된 domain을 반환하므로 모듈은 iommufd 아래서도 동작할 것으로 예상 — 검증 필요)
2. **PAL 대기**: upstream ②(PAL) 병합 + NVIDIA exporter 구현(⑤)을 기다림.
3. **udmabuf-PAL 브릿지**: ② 병합 후 udmabuf를 PAL-capable exporter로 확장.

### 6.3 논의 안건 (Simon과)

1. #38에 GPU→storage(P2P read) 방향 검증 추가 — 본 구현이 이 방향의 검증 기법(패턴 교차 2라운드 + host read-back)을 보유.
2. #38에 "GPU가 vfio-pci에 바인딩되는 동안 CUDA 불능"임을 커밋/주석으로 명시 제안 — 오독 방지.
3. CUDA 공백을 메우는 interim 경로로 하이브리드(6.2의 1) 설계 논의.
4. #39 머지 시점과 본 브랜치 rebase 조율.

---

## 7. 요약

```
#37: dmamem + iommufd 헬퍼 = upcie 의 iommufd 기반 공사 (phys_lut 없는 연속 IOVA 모델)
#38: VFIO BAR dma-buf -> IOAS_MAP_FILE 로 NVMe->VRAM 직접 DMA 를 mainline 만으로 증명.
     단 GPU 를 vfio-pci 에 내주므로 CUDA 불능 = 배관 증명이지 GPGPU 워크플로 아님.
#39: type1/iommufd 를 UPCIE_VFIO_MODE 런타임 선택으로 통합.

의미:  비교 보고서 §6 로드맵의 자체 수행 항목이 Simon 에 의해 구현됨.
       CUDA 소유 GPU 메모리 경로는 여전히 본 구현(udmabuf + upcie_iommu_map.ko)이 유일.
       다음 단계는 두 작업의 결합(dmamem CUDA constructor) 또는 upstream PAL 대기.
```
