# upcie iommufd PR(#37~#39) 상세 분석 보고서 (코드 수준)

개요 보고서(`docs/iommufd_prs_analysis.md`)의 후속으로, 세 PR의 **패치 원문 전체**를 기준으로 커밋 단위·함수 단위까지 분석한다. 이하 PA = physical address.

분석 대상 규모:

| PR | 커밋 수 | 패치 라인 | 주요 산출물 |
|---|---|---|---|
| #37 | 5 | ~2,175 | `iommufd.h`(+366), `dmamem*.h`(+541), `nvme_controller_dmamem.h`(+395) |
| #38 | 5 | ~1,026 | `dmamem_dmabuf.h`(+83), BAR export(+53), VRAM smoketest(+247+96) |
| #39 | 2 | ~696 | `nvme_controller_vfio_pci.h` 신설, 드라이버 모드 통합 (+175/−198) |

---

## 1. PR #37 — iommufd 헬퍼 + dmamem (기반)

### 1.1 커밋 구성

| # | 커밋 | 내용 |
|---|---|---|
| 1/5 | feat(iommufd): user-space helper header | `/dev/iommu` ioctl + vfio-cdev bind/attach 래퍼 |
| 2/5 | feat(dmamem): sibling to hostmem | dmamem 타입 + heap + memfd 생성자 |
| 3/5 | ci(dmamem): smoketest task | cijoe 태스크, deploy에 `iommu show`·`modprobe iommufd` 추가 |
| 4/5 | feat(nvme): controller extension | `nvme_controller_dmamem` + IDENTIFY smoketest |
| 5/5 | feat(driver): wire dmamem backend | `UPCIE_BACKEND=dmamem` (이 시점 IO qpair 미지원, IDENTIFY까지만) |

### 1.2 `include/upcie/iommufd.h` — ioctl 래퍼 (366줄)

헤더 주석에 객체 관계가 요약되어 있다:

```
 /dev/iommu (iommufd)
       |
    IOAS, I/O address space         <- IOMMU_IOAS_ALLOC
       |   map: user_va or fd -> IOVA  (IOMMU_IOAS_MAP / IOMMU_IOAS_MAP_FILE)
       |
  attached device                   <- VFIO_DEVICE_ATTACH_IOMMUFD_PT
       |
  vfio device cdev (/dev/vfio/devices/vfioN)
```

타입은 두 개뿐이다:

```c
struct iommufd        { int fd; };                    // /dev/iommu 핸들
struct iommufd_device { int fd; uint32_t devid; };    // vfio cdev + BIND가 준 devid
```

함수 ↔ ioctl 대응:

| 함수 | ioctl | 비고 |
|---|---|---|
| `iommufd_open/close` | open/close `/dev/iommu` | close가 소유한 IOAS·매핑 전부 해제 |
| `iommufd_ioas_alloc` | `IOMMU_IOAS_ALLOC` | `out_ioas_id` 반환 |
| `iommufd_destroy` | `IOMMU_DESTROY` | id로 객체 파괴 |
| `iommufd_ioas_map` | `IOMMU_IOAS_MAP` | user_va 기반. `iova`는 in/out — `IOMMU_IOAS_MAP_FIXED_IOVA` 플래그일 때만 입력값 존중 |
| `iommufd_ioas_map_file` | `IOMMU_IOAS_MAP_FILE` | fd(memfd/dma-buf) 기반. 동일한 FIXED_IOVA 규약 |
| `iommufd_ioas_unmap` | `IOMMU_IOAS_UNMAP` | |
| `iommufd_device_open/close` | open/close cdev | close가 detach+unbind까지 수행 |
| `iommufd_device_bind` | `VFIO_DEVICE_BIND_IOMMUFD` | `out_devid` 저장 |
| `iommufd_device_attach/detach` | `VFIO_DEVICE_ATTACH/DETACH_IOMMUFD_PT` | pt_id = IOAS id |

**호환성 처리**: `#ifdef IOMMU_IOAS_MAP_FILE`로 감싸고, 6.14 미만 UAPI 헤더에서는 `-ENOTSUP`을 돌려주는 stub을 제공한다. 호출부는 `#ifdef` 없이 항상 같은 함수를 부르고, 실패 시 `iommufd_ioas_map(user_va)`로 폴백한다.

**주목**: 헤더 주석에 이미 "A dma-buf fd, **e.g. exported from CUDA device memory**, can be mapped with iommufd_ioas_map_file"이라고 적혀 있다. 즉 Simon은 CUDA dma-buf를 최종 소비 대상으로 상정하고 있으나, 현 mainline에서는 그 경로가 막혀 있음(§2.5)을 별도 커밋에서 인정한다.

### 1.3 `dmamem.h` — 공유 타입 (155줄)

```c
enum dmamem_backing { UNKNOWN, MEMFD, DMABUF };

struct dmamem {
    int fd;                 // memfd 또는 dma-buf; dmamem이 소유
    void *cpu_va;           // CPU 매핑 불가한 backing이면 NULL
    uint64_t base_iova;     // IOAS 안의 base IOVA
    uint32_t ioas_id;
    enum dmamem_backing backing;
};
```

주소 변환은 두 함수로 끝난다:

```c
iova = dmem->base_iova + offset;                          // dmamem_offset_to_iova
iova = dmem->base_iova + (vaddr - dmem->cpu_va);          // dmamem_va_to_iova (cpu_va 필수)
```

`dmamem_destroy`는 `iommufd_ioas_unmap` → `munmap(cpu_va)` → `close(fd)` 순서로 정리한다. **본 구현의 hostmem/phys_lut와의 대비가 이 구조체 하나에 응축**되어 있다: 페이지별 PA 배열 대신 `(base_iova, size)` 창 하나.

### 1.4 `dmamem_memfd.h` — memfd 생성자 (127줄)

```c
memfd_create("dmamem", MFD_HUGETLB | MFD_HUGE_2MB(또는 _1GB))
  -> ftruncate(size) -> mmap(RW, SHARED) -> mlock
  -> iommufd_ioas_map_file(memfd, ...) -> base_iova
```

- `MFD_HUGE_2MB/1GB` 매크로가 없는 오래된 헤더 대비 자체 정의 포함.
- `mlock` 요구 → 실행에 `ulimit -l unlimited`(cijoe task에서 실제로 설정)와 IPC_LOCK 필요.
- hugepage인 이유: pin 대상 페이지 수 절감 + IOAS 매핑 시 커널이 큰 IOPTE 사용 가능.

### 1.5 `dmamem_heap.h` — 오프셋 서브할당자 (259줄)

`(base_iova, size)` 창 위에서 동작하는 최소 할당자. 핵심 설계: **CPU VA가 없는 backing(VRAM/MMIO)에서도 동작**하도록, 반환 단위가 포인터가 아니라 **offset**이고, IOVA는 항상 얻을 수 있되 VA는 memfd일 때만 얻는다. 이 결정 덕에 #38에서 VRAM 위에도 같은 heap을 쓸 수 있다.

### 1.6 `nvme_controller_dmamem.h` — cdev 경로 NVMe open (395줄)

`nvme_controller_open_dmamem(ctrlr, ctx, iommufd, ioas_id, heap, cdev_path)` 흐름:

```
iommufd_device_open(cdev) -> iommufd_device_bind -> iommufd_device_attach(ioas)
-> bus master enable (VFIO_PCI_CONFIG_REGION_INDEX 에 config-space write)
-> BAR0 region info + mmap
-> SQ/CQ 를 dmamem_heap 에서 carve -> sq_iova/cq_iova = heap_at_iova(offset)
-> AQA/ASQ/ACQ 프로그래밍 -> CC enable -> CSTS.RDY 대기
```

컨텍스트 구조체가 소유권을 명시한다: `struct nvme_dmamem_ctx { struct iommufd_device dev; void *bar0; size_t bar0_size; }`. 기존 `nvme_qpair` submit/reap 프리미티브는 heap 종류에 무관(heap-agnostic)해서 그대로 재사용 — 변경 없이 PRP에 IOVA만 들어간다.

### 1.7 설계 판단 — 왜 hostmem 확장이 아니라 sibling인가

커밋 메시지가 근거를 명시한다 (원문 요지):

> hostmem은 uio_pci_generic + iommu=pt/off 세계에서 여전히 옳다 — 거기선 PA 단편화가 디바이스에 보이고 phys_lut[]가 그걸 숨긴다. vfio+iommufd에서는 IOMMU_IOAS_MAP_FILE 한 번이 물리 배치와 무관한 연속 IOVA 창을 주므로 단편화 장부가 사라지고 제출 경로가 `iova = base_iova + offset`으로 붕괴한다. **다른 세계, 다른 모듈.**

본 비교 보고서의 "UIO=PA 세계 / VFIO=IOVA 세계" 구분과 정확히 같은 논리다.

---

## 2. PR #38 — dma-buf 생성자 + NVMe→VRAM PoC

### 2.1 커밋 구성

| # | 커밋 | 내용 |
|---|---|---|
| 1/5 | dmamem_from_dmabuf | dma-buf fd → IOAS import 생성자 |
| 2/5 | vfio_device_bar_export_dmabuf | BAR 조각 → dma-buf fd |
| 3/5 | NVMe DMA into GPU VRAM | 공유 IOAS + IDENTIFY→VRAM PoC |
| 4/5 | IO qpair on dmamem path | CREATE_IO_CQ/SQ + admin sync 헬퍼 |
| 5/5 | real disk READ | READ LBA0 → VRAM + fingerprint 확인 |

### 2.2 `dmamem_from_dmabuf` — 소유권 규약이 명확한 생성자

```c
int dmamem_from_dmabuf(dmem, iommufd, ioas_id, dmabuf_fd, size)
{
    dmem->fd = dmabuf_fd; dmem->backing = DMAMEM_BACKING_DMABUF;
    // CPU 매핑은 exporter 마음: 되면 cpu_va, 안 되면 NULL (진단용, 실패해도 무방)
    cpu_va = mmap(dmabuf_fd, ...);
    err = iommufd_ioas_map_file(iommufd, ioas_id, dmabuf_fd, 0, size,
                                READABLE|WRITEABLE, &dmem->base_iova);
}
```

계약이 문서화되어 있다:
- **성공 시 dmamem이 fd 소유권을 가져가고**(destroy에서 close), 실패 시 호출자가 유지.
- cpu_va는 exporter 의존 — "udmabuf-backed는 CPU-mappable, GPU 런타임의 VRAM-backed는 보통 아님".
- 헤더 주석: 이 함수가 "**MAP_FILE이 오늘 dma-buf를 받아주는가?**"라는 질문을 실행 커널에 직접 던지는 진입점이며, 실패 errno로 병목(커널/exporter/IOAS 설정)을 특정한다. — 탐침(probe)으로서의 성격을 명시한 것.

### 2.3 `vfio_device_bar_export_dmabuf` — BAR → dma-buf (Leon ①의 소비자)

가변 길이 구조체 조립이 핵심이다:

```c
bufsz = sizeof(vfio_device_feature) + sizeof(vfio_device_feature_dma_buf)
      + sizeof(vfio_region_dma_range);
feat->flags = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_DMA_BUF;
dbuf->region_index = region_index;   // 예: VFIO_PCI_BAR1_REGION_INDEX
dbuf->open_flags   = O_RDWR | O_CLOEXEC;
dbuf->nr_ranges    = 1;
range = { .offset = offset, .length = length };
fd = ioctl(device_fd, VFIO_DEVICE_FEATURE, feat);   // 반환값이 dma-buf fd
```

주석이 커널 내부 메커니즘까지 짚는다: 이 fd를 MAP_FILE이 받아주는 이유는 exporter가 vfio-pci라서 **`vfio_pci_dma_buf_iommufd_map`이라는 private interconnect**로 처리되기 때문 — 즉 PAL 같은 일반 메커니즘이 아니라 vfio-pci↔iommufd 간 전용 통로다. 본 보고서 §5의 "PAL 없이 ③이 동작하는 이유"와 일치하며, **왜 다른 exporter(CUDA)는 안 되는지**도 이 지점에서 명확해진다.

### 2.4 VRAM smoketest — 두 디바이스가 한 IOAS를 공유

```
iommufd_open -> ioas_alloc                        (IOAS 하나)
GPU:  cdev open -> bind -> attach(ioas)
      -> bar_export_dmabuf(bar_index, 0, bar_size)  -> dma-buf fd
      -> dmamem_from_dmabuf(...)                    -> VRAM 창: base_iova_G
      -> (검증용) vfio fd 로 BAR 를 CPU mmap
NVMe: nvme_controller_open_dmamem(ioas)             -> admin heap 창: base_iova_H

[Step 1] IDENTIFY(0x06, CNS=1), PRP1 = base_iova_G + 0x2000
         -> CQE 확인 -> BAR mmap 으로 SN/MN/FR 문자열 출력, 전부 0 이면 FAIL
[Step 2] create_io_qpair_dmamem(depth 32)
         READ(0x02) LBA0, NLB=7(8블록=4KiB), PRP1 = base_iova_G + 0x4000
         -> CQE 확인 -> 첫 16바이트 fingerprint 출력
         -> all-zero 면 "inconclusive"(디스크 LBA0 이 0 일 수도), non-zero 면 OK
```

세부 사항:
- GPU도 IOAS에 **attach까지** 한다(단순 export만이 아님). attach는 BAR export의 전제 조건은 아니지만, cdev 열기·bind와 함께 일괄 수행.
- IDENTIFY 전에 대상 VRAM 4KiB를 BAR mmap으로 0으로 밀어둔다 — "직전 렌더러가 남긴 값" 오탐 방지. 주석에 "uncached라 느리지만 작은 영역이라 무방"이라고 명시.
- IO qpair는 `nvme_admin_sync_dmamem`(request pool 없는 동기 admin submit+reap 헬퍼)으로 CREATE_IO_CQ(0x5)/CREATE_IO_SQ(0x1)를 프로그래밍. SQ/CQ 자체도 dmamem_heap에서 carve.

### 2.5 검증 강도 평가 (본 구현 테스트와 대비)

| 항목 | #38 | 본 구현 테스트 |
|---|---|---|
| storage→GPU | IDENTIFY 문자열 + READ 첫 16B **non-zero 여부** | 패턴 A→B 교체 R-W-R liveness, **바이트 단위 전량 비교** |
| GPU→storage (P2P read) | **없음** | 2라운드 × (host read-back + GPU 왕복) 전량 비교 |
| all-zero 처리 | "inconclusive" 경고로 넘어감 | 기대 패턴을 먼저 기록하므로 모호성 없음 |

#38의 검증은 "DMA가 일어났는가" 수준이고, **데이터 무결성 검증이 아니다**. LBA0에 미리 알려진 패턴을 써두는 단계가 없어서 all-zero가 원천적으로 모호하다. 본 구현의 기법(사전 패턴 기록 후 비교)을 이식하면 두 줄로 해소된다 — 리뷰 코멘트 후보 1순위.

### 2.6 GPU 바인딩(YAML)의 함의

`bind_gpu_group` 스텝이 GPU의 IOMMU group 전체를 순회하며 기존 드라이버를 unbind하고 vfio-pci로 넘긴다(브리지 class 0x06\*만 제외). **nvidia.ko가 이 unbind의 대상이라는 언급은 커밋·주석 어디에도 없다.** 이 구성의 결과(그 GPU에서 CUDA 불능)는 개요 보고서 §4에서 상술했다.

---

## 3. PR #39 — type1/iommufd 통합

### 3.1 모드 선택 (patch 1/2)

```c
enum vfio_mode { VFIO_MODE_AUTO, VFIO_MODE_TYPE1, VFIO_MODE_IOMMUFD };

vfio_mode_from_env():  getenv("UPCIE_VFIO_MODE") 파싱, 미지정/오타 -> AUTO(+경고)

AUTO 판정:
  #ifdef IOMMU_IOAS_MAP_FILE            // 빌드 시 UAPI 헤더가 6.14+ 인가
      access("/dev/iommu", R_OK|W_OK)   // 런타임에 iommufd 가 있는가
  둘 다 참 -> NVME_BACKEND_VFIO_IOMMUFD, 아니면 NVME_BACKEND_VFIO_TYPE1
```

판정이 **빌드타임(#ifdef) + 런타임(access) 이중**이라는 점이 실무적이다 — 새 헤더로 빌드했지만 옛 커널에서 도는 경우, 옛 헤더로 빌드한 경우 모두 type1로 안전하게 수렴한다. cijoe에 `dmamem_driver_run_type1.yaml`이 추가되어 **type1 경로가 회귀 비교용으로 계속 시험**된다.

### 3.2 공유 bring-up 헬퍼 (patch 2/2)

`nvme_controller_vfio_pci.h` 신설, 4개 함수:

| 함수 | 역할 |
|---|---|
| `nvme_vfio_pci_bus_master_enable` | config region에 write로 BM=1 |
| `nvme_vfio_pci_acquire_bar0` | BM enable + BAR0 region info + mmap + ctrlr 채움 (합성 헬퍼) |
| `nvme_controller_reset_via_bar0` | CAP 읽기 → timeout 도출 → CC=0 → CSTS.RDY=0 대기 |
| `nvme_controller_enable_via_bar0` | CC 표준값 프로그래밍 → RDY=1 대기 (AQA/ASQ/ACQ는 호출자 책임) |

헤더 주석의 설계 문장: "**변하지 않는 중간(invariant middle)은 여기 산다. 변하는 부분(fd를 어떻게 얻는지, 메모리를 어떻게 매핑하는지)은 각 backend에 남는다.**" 결과적으로 type1과 iommufd의 차이는 (a) fd 획득: group vs cdev, (b) 매핑: `VFIO_IOMMU_MAP_DMA` vs `IOMMU_IOAS_MAP_FILE` — 딱 두 가지로 수렴한다.

### 3.3 본 브랜치와의 충돌 지점

`nvme_controller_vfio.h`에서 bus master/BAR0/CC·CSTS 시퀀스가 **삭제되고 공용 헬퍼 호출로 대체**된다. 본 브랜치(`vfio_cudamem_final`)의 nvme-vfio 커밋이 정확히 그 시퀀스 주변에 UPCIE_DEBUG 마일스톤을 삽입했으므로, #39 머지 시 해당 hunk는 충돌한다. rebase 시 마일스톤을 공용 헬퍼(`nvme_controller_vfio_pci.h`) 쪽으로 옮기는 편이 두 backend 모두에서 디버그가 살아나므로 오히려 개선이다.

---

## 4. 코드 수준 종합 평가

### 4.1 강점

- **오류 표면화 철학**: 모든 래퍼가 `-errno`를 그대로 올리고, smoketest는 "실패한 ioctl이 곧 진단"이 되도록 설계됨(#37 커밋 3의 "green deploy that ran nothing" 방지 포함).
- **소유권 규약 명시**: dmamem의 fd 소유권 이전, ctx가 소유한 dev/bar0, close의 연쇄 해제(detach→unbind) 등이 주석으로 계약화됨.
- **경계 호환성**: MAP_FILE stub(-ENOTSUP), MFD_HUGE 매크로 자체 정의, 빌드+런타임 이중 판정 — 옛 커널에서 조용히 깨지지 않음.
- **재사용 극대화**: qpair submit/reap이 heap-agnostic이라 dmamem 도입에도 무변경. heap이 offset 기반이라 VRAM(무 CPU VA)에도 동작.

### 4.2 잠재 이슈 / 논의거리

1. **READ 검증 약함** — fingerprint non-zero는 무결성 증명이 아님(§2.5). 사전 패턴 기록 + 전량 비교 제안.
2. **GPU→storage 방향 부재** — P2P read(NVMe가 GPU를 읽는 방향)는 플랫폼 민감성이 커서 별도 검증 가치가 있음. 본 구현의 2라운드 기법 이식 가능.
3. **BAR 전체 export** — smoketest가 `(0, bar_size)` 한 range로 BAR 전부를 NVMe에 노출한다. PoC로는 타당하나, 실사용이라면 필요한 창만 export하는 것이 격리 취지에 부합.
4. **IOVA 할당 방식 차이** — dmamem은 커널 할당 IOVA(FIXED_IOVA 미사용). 본 구현의 host identity(IOVA=PA) 가정과 다르므로, 기존 hostmem 기반 PRP 코드와 섞을 때 주의. (iommufd에도 `IOMMU_IOAS_MAP_FIXED_IOVA`가 있어 고정 IOVA가 필요하면 사용 가능)
5. **CUDA 공백** — `dmamem_from_dmabuf`는 "any exporter"를 선언하지만, 현 mainline에서 실제 통과하는 exporter는 vfio-pci뿐(private interconnect). CUDA dma-buf를 넣으면 MAP_FILE이 거부한다. 주석의 "udmabuf host memory"도 udmabuf가 일반 memfd 기반일 때의 이야기이며, GPU VRAM import 케이스가 아니다.

### 4.3 본 구현과의 결합 설계 (하이브리드 구체화)

`struct dmamem`의 계약은 `(fd, cpu_va?, base_iova, size, backing)` 뿐이므로, **CUDA 생성자**를 다음 형태로 끼울 수 있다:

```
dmamem_from_cuda(dmem, iommufd, ioas_id, cuda_dmabuf_fd, size):
    udmabuf 로 phys_lut 추출
    UPCIE_IOMMU_MAP ioctl (본 모듈) 로 NVMe 의 attach 된 domain 에 iova_base 고정 매핑
    dmem->base_iova = iova_base; dmem->backing = DMAMEM_BACKING_CUDA(신설)
```

- 전제 1: `iommu_get_domain_for_dev(NVMe)`가 iommufd HWPT domain도 반환함(검증 필요하나 API 계약상 성립 예상).
- 전제 2: 본 모듈의 iova_base가 IOAS의 커널 할당 영역과 충돌하지 않아야 함 → IOAS에 같은 범위를 `IOMMU_IOAS_MAP_FIXED_IOVA`로 예약해 두거나, iommufd의 IOVA allocator 범위를 조회(IOMMU_IOAS_IOVA_RANGES)해 회피.
- 이렇게 하면 제출 경로는 dmamem 규약(`base_iova + offset`) 그대로이고, PAL(②) + NVIDIA exporter(⑤)가 성숙하면 이 생성자 내부만 `iommufd_ioas_map_file(cuda_fd)`로 교체하면 된다 — **교체 지점이 함수 하나로 국소화**된다.

---

## 5. NVIDIA exporter(⑤) 코드 분석 — 왜 CUDA dma-buf가 iommufd에 안 되나

전환 로드맵의 최대 관문 ⑤(NVIDIA open KMD의 PAL 지원)를 실제 소스(`NVIDIA/open-gpu-kernel-modules`, `kernel-open/nvidia/nv-dmabuf.c` · `nv-pci.c`)로 확인했다.

### 5.1 export 경로 분기

`nv_dma_buf_map()`이 두 경로 중 하나를 고른다:

```c
if (priv->nv->coherent && priv->mapping_type == NV_DMABUF_EXPORT_MAPPING_TYPE_DEFAULT)
    sgt = nv_dma_buf_map_pages(...);   // struct-page 기반 scatterlist, dma_map_sg
else
    sgt = nv_dma_buf_map_pfns(...);    // framebuffer(BAR) PFN 기반
```

- 일반 PCIe dGPU(L40S 등)는 framebuffer 메모리라 struct page가 없어 **`nv_dma_buf_map_pfns()`** 를 탄다.
- `map_pfns`는 `sg_set_page(sg, NULL, ...)`로 page 없이 두고, `sg->dma_address = nv_dma_map_peer(...)`로 채운다. **원천은 BAR의 물리(PFN)지만, importer 디바이스 기준으로 매핑된 `dma_addr_t`로 감싸서** 내준다.

즉 NVIDIA는 "물리에서 출발해 importer 문맥으로 번역"하는 legacy dma-buf 계약 그대로다. importer가 udmabuf(무변환)이면 그 값이 물리와 같아져 우리 phys_lut이 성립한다(본 구현). importer가 iommufd이면 iommufd가 원하는 것은 "번역 전 raw PA 목록(PAL)"인데, 이 경로는 PAL이 아니라 sg의 dma_addr_t라 소비할 수 없다.

### 5.2 direct-PCI(FORCE_PCIE) 게이트 — L40S는 첫 줄에서 탈락

`FORCE_PCIE` mapping type을 쓰면 attach 시 topology 검사를 통과해야 한다(`nv-pci.c`):

```c
NvBool nv_pci_is_valid_topology_for_direct_pci(nv_state_t *nv, struct pci_dev *peer)
{
    struct pci_dev *pdev0 = to_pci_dev(nv->dma_dev->dev);
    if (!nv->coherent)                                   // ← 일반 PCIe dGPU = 여기서 FALSE
        return NV_FALSE;
    if (pdev0->dev.iommu_group == peer->dev.iommu_group) // 같은 IOMMU group
        return NV_TRUE;
    if (peer->dev.iommu_group == NULL)                   // peer가 group 없으면
        return nv_pci_has_common_pci_switch(nv, peer);   // 공통 upstream 스위치 요구
    return NV_FALSE;
}
```

- **`!nv->coherent` → 즉시 FALSE.** `coherent`는 NVLink/C2C 통합 플랫폼(Grace Superchip) 또는 framebuffer 없는 특수칩(Thor GB10B)에서만 참이다. 따라서 **일반 PCIe dGPU에서는 direct-PCI export 자체가 `-ENOTSUPP`로 막힌다.**
- 흥미롭게도 coherent 경로의 2·3번 조건(같은 IOMMU group / 공통 스위치)은 본 보고서·비교 보고서가 정리한 "P2P 성패는 ACS·공통 스위치 topology가 좌우"라는 분석과 동일한 논리를 NVIDIA가 코드로 구현한 것이다.

### 5.3 결론 — ⑤의 실제 거리

| 항목 | NVIDIA 현황(코드 확인) |
|---|---|
| 물리 export 로직 | 있음 — `nv_dma_buf_map_pfns`, 일반 dGPU의 기본 경로 |
| 표준 PAL 인터페이스로 노출 | ❌ 없음 (sg의 dma_addr_t로 감싸 반환) |
| direct-PCI(FORCE_PCIE) | ❌ `!coherent`로 일반 dGPU 차단, Grace/Thor 전용 |
| 강제 활성 module param/regkey | 공개 코드에 없음 (`static_phys_addrs`는 closed RM `nv-kernel.o`) |

시사점: ⑤는 "NVIDIA가 물리를 처음부터 계산" 문제가 아니라 **(a) 이미 있는 PFN 경로를 PAL mapping type으로 노출 + (b) coherent 게이트를 일반 PCIe로 완화**의 문제다. 기술적 거리는 가까우나, NVIDIA가 PCIe dma-buf P2P를 의도적으로 coherent SoC에 한정하고 일반 dGPU는 자사 스택(nvidia-peermem/GPUDirect)으로 유도하는 정황이라, 단기 가능성은 낮다. 이는 본 구현(udmabuf + `upcie_iommu_map.ko`)이 L40S에서 CUDA를 유지한 채 P2P를 얻는 사실상 유일 경로라는 위상을 강화한다.

---

## 6. 요약

```
#37: iommufd.h(래퍼 12종, MAP_FILE stub 폴백) + dmamem(창 하나, offset 산술)
     + memfd 생성자(HUGETLB+mlock) + cdev 경로 NVMe open. "다른 세계, 다른 모듈."
#38: BAR export(VFIO_DEVICE_FEATURE_DMA_BUF, private interconnect 로 MAP_FILE 통과)
     + dma-buf 생성자(fd 소유권 규약) + 공유 IOAS 에서 IDENTIFY/READ 를 VRAM 에 착지.
     검증은 non-zero fingerprint 수준(무결성 아님), GPU→storage 방향 없음.
#39: 빌드+런타임 이중 판정의 auto 모드, bring-up 4종 공용화.
     본 브랜치 nvme-vfio 커밋과 충돌 예정 — 마일스톤을 공용 헬퍼로 이전이 정답.

결합: struct dmamem 계약에 맞춘 dmamem_from_cuda(udmabuf + 본 모듈) 생성자가
      하이브리드의 최소 침습 지점. FIXED_IOVA 예약 또는 IOVA_RANGES 조회로
      커널 할당 IOVA 와의 충돌만 관리하면 된다. PAL 성숙 시 함수 내부만 교체.
```
