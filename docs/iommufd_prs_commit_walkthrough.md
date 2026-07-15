# Simon PR #37~#40 커밋별 코드 리딩 가이드

작성일 2026-07-15. 검토 대상 head는
[관계 문서](./IOMMUFD_UPSTREAM_SIMON_PRS_RELATION.md)와 동일하다
(#37 `cbcccc9`, #38 `ad60627`, #39 `599d267`, #40 `a4f9a4f`).
2026-07-13 이후 force-push는 없다.

이 문서는 커밋 순서대로 코드를 직접 읽을 때 옆에 두는 가이드다. 커밋마다
세 가지를 적는다.

1. **들어오는 것**: 이 커밋이 실제로 추가/변경하는 코드
2. **읽을 위치**: 파일과 함수 기준의 리딩 포인트
3. **확인할 것**: 이 커밋에서 검증해야 할 결함 또는 계약

이슈의 상세 근거는 [deep dive](./iommufd_prs_deep_dive.md)에 있다. 아래에서
`[DD #NN-k]`는 deep dive의 "PR #NN 먼저 고쳐야 할 부분 k번"을 뜻하고,
`[신규]`는 이번 재검토(2026-07-15)에서 새로 확인한 항목이다.

diff를 뜨는 기준은 stacked base다.

```bash
git fetch origin 'refs/pull/37/head:refs/remotes/origin/pr/37' \
                 'refs/pull/38/head:refs/remotes/origin/pr/38' \
                 'refs/pull/39/head:refs/remotes/origin/pr/39' \
                 'refs/pull/40/head:refs/remotes/origin/pr/40'
git log --reverse origin/main..origin/pr/37       # 커밋 순서 확인
git show <commit>                                  # 커밋 하나씩
```

---

## PR #37: iommufd + dmamem 기반 (5 커밋, +1963)

### 37-1. `9beffae` feat(iommufd): add user-space helper header (+462)

**들어오는 것**: `include/upcie/iommufd.h` 단일 헤더. `struct iommufd`
(`/dev/iommu` fd)와 `struct iommufd_device`(vfio cdev fd + devid) 두
핸들, 그리고 ioctl 이름 그대로의 wrapper 10개
(`ioas_alloc/destroy/map/map_file/unmap`, `device_open/close/bind/attach/detach`).

**읽을 위치**
- 헤더 상단 주석의 객체 관계도(iommufd → IOAS → attached device → cdev)
- 호환성 gating 두 층: `UPCIE_HAVE_IOMMUFD`(linux/iommufd.h 존재 +
  `VFIO_DEVICE_BIND_IOMMUFD`)와 별도의 `#ifdef IOMMU_IOAS_MAP_FILE`
- `iommufd_ioas_map[_file]`의 `iova`가 in/out 파라미터인 것
  (FIXED_IOVA 미설정 시 커널이 골라서 반환)

**확인할 것**
- stub이 `-ENOTSUP`을 반환하므로 **컴파일 성공 ≠ 기능 존재**. 런타임
  폴백은 호출자 책임인데 #37의 constructor는 폴백하지 않는다(37-2 참고).
- `iommufd_close()`가 fd 소유 IOAS/mapping을 전부 정리한다는 주석 —
  이후 모든 teardown 순서 논의의 전제.
- [신규] 모든 `open()`에 `O_CLOEXEC`가 없다. cijoe처럼 fork/exec하는
  오케스트레이션에서 fd가 자식으로 샌다. 심각도 낮음.

### 37-2. `26d1646` feat(dmamem): DMA-capable memory abstraction (+687)

**들어오는 것**: `dmamem.h`(descriptor + offset→IOVA 산술),
`dmamem_memfd.h`(hugepage memfd constructor), `dmamem_heap.h`(offset
기반 first-fit allocator), memfd smoketest. **이 커밋이 스택 전체의
결함 밀도가 가장 높다.**

**읽을 위치**
- `dmamem_memfd.h:39-127` `dmamem_from_memfd()`: memfd_create →
  ftruncate → mmap → mlock → memset(pin 유도) → `IOMMU_IOAS_MAP_FILE`
- `dmamem_heap.h:123-192` `dmamem_heap_alloc_aligned()`: front gap
  carve → tail carve → 블록 할당
- `dmamem.h:106-122` fast path 두 개(`offset_to_iova`, `va_to_iova`)

**확인할 것**
- [DD #37-1, High] `dmamem_heap.h:142` `if (b->size < front_gap + size)` —
  `front_gap + size`가 `size_t`를 넘으면 wrap. `SIZE_MAX` 요청이 성공해
  겹치는 블록이 생긴다. 뺄셈 형태로 고쳐야 함.
- [DD #37-2, Med] `dmamem_heap.h:139` bitmask 정렬식은 2의 거듭제곱
  전용인데 API는 임의 alignment를 받는다.
- [DD #37-3, High] `dmamem_memfd.h:50` `size % hugepgsz`를 hugepgsz==0
  검사 전에 계산 → SIGFPE.
- [DD #37-4, Med] `dmamem_memfd.h:64-69`에서 mapping **전에** iommufd,
  ioas_id, size를 descriptor에 기록. 실패한 객체에 `dmamem_destroy()`를
  부르면 map 안 한 IOVA 0을 unmap하려 한다(destroy의 가드는
  `iommufd && size`뿐, `dmamem.h:137`).
- [신규, Low] `dmamem_heap_free()`(`dmamem_heap.h:204-238`)는 블록이
  할당 상태인지 확인하지 않는다. double-free가 조용히 통과하고, 그
  offset이 재할당된 뒤라면 살아 있는 할당을 해제한다.
- [신규, Low] `dmamem_va_to_iova()`의 `assert(cpu_va)`는 NDEBUG 빌드에서
  사라진다 — 검증이 전무해지는 지점(#40의 [DD #40-10]과 같은 계열).
- memfd smoketest의 IOVA 비교는 같은 `base_iova + offset` 산술의 재계산
  이라 독립 검증이 아니다.

### 37-3. `8fe3663` ci(dmamem): smoketest task + deploy tweaks (+24)

**들어오는 것**: cijoe task 1개 + deploy에 `iommu show`/`modprobe iommufd`.

**확인할 것**: 이 task는 수동 하드웨어 task이며 표준
`.github/workflows/verify.yml`에서 호출되지 않는다. `meson test`는 여전히
0개. "테스트가 추가됐다"로 읽으면 과대평가.

### 37-4. `5a18811` feat(nvme): controller extension + IDENTIFY smoketest (+592)

**들어오는 것**: `nvme_controller_dmamem_vfio.h` — cdev open → bind →
attach → BME → BAR0 mmap → CC.EN=0/1 → Admin SQ/CQ(dmamem_heap에서
64 KiB씩) → RDY 대기. IDENTIFY smoketest 포함.

**읽을 위치**
- `nvme_qpair_dmamem_init()`(h.145-187): 64 KiB 고정 할당, depth는
  qp 필드에만 기록
- `nvme_controller_open_dmamem_vfio()`의 `fail:` 라벨(h.337-344)
- `nvme_dmamem_vfio_ctx_close()`(h.54-84): munmap → detach → close 순서

**확인할 것**
- [DD #37-6, Med→#38에서 High로 승격] depth 미검증: 64 KiB SQ에는 64B
  entry 1024개가 한계. depth 0이면 `(tail+1) % depth`에서 0 나눗셈.
  `CAP.MQES` 대조도 없음. #37 내부 caller는 depth 256 고정이라 안 밟지만
  public API 계약은 이미 깨져 있다.
- [DD #37-5, High] `CC.EN=1` 후 RDY timeout이 나면 fail 경로가 **CC를
  다시 disable하지 않고** AQ를 heap에 반환한 뒤 detach한다. 순서는
  free → detach라서, detach가 DMA를 끊기 전에 메모리가 재사용 가능
  상태가 된다.
- [DD #37-7] `qp->rpool == NULL`: 기존 `nvme_qpair_submit_sync*()`를 이
  qpair에 쓰면 NULL 역참조. smoketest가 CID 1을 수동 지정하는 이유.
  #40 첫 커밋이 보완.
- `qp->sq = dmamem_heap_at_va(...)` 직후 `memset(qp->sq, ...)` —
  cpu_va 없는 backing이면 NULL memset([DD #40-8]의 근원이 여기).

### 37-5. `cbcccc9` feat(driver): wire the dmamem backend (+198)

**들어오는 것**: `upcie_nvme_driver.c`에 `NVME_BACKEND_DMAMEM`
(`UPCIE_BACKEND=dmamem`으로 opt-in), `nvme_dmamem_state`의 `*_alive`
플래그와 역순 cleanup, sysfs에서 cdev 경로를 찾는
`resolve_vfio_cdev()`.

**확인할 것**
- [DD #37-9, Low] `nvme_init()`의 dmamem 분기가 이미 음수인 err를
  `return -err`로 **양수** 반환 — negative errno 관례 위반.
- [DD #37-8, Med] `main()`이 backend 선택 전에 `rte_init()`으로 128 MiB
  legacy hostmem_heap을 요구. dmamem 경로는 그걸 쓰지 않는데 hugepage
  부족이면 진입 자체가 실패.
- cleanup 순서 자체(buf → ctrlr → heap → dmem → ioas → iommufd)는
  올바르다. identify가 timeout으로 실패했을 때 buf를 controller
  disable **전에** heap free하는 것만 [DD #38-1]과 같은 계열.

---

## PR #38: dma-buf + VFIO BAR P2P (5 커밋, +797)

> 주의: 중간 커밋 `ecf3163`, `9b105d0`은 **빌드가 깨진다**(smoketest가
> 6-인자 open에 8개 전달). 마지막 커밋에서야 고쳐지므로 이 두 커밋은
> bisect point로 쓸 수 없다.

### 38-1. `a6974cf` feat(dmamem): dma-buf constructor (+85)

**들어오는 것**: `dmamem_dmabuf.h`의 `dmamem_from_dmabuf()` — 기존
dma-buf fd를 `IOMMU_IOAS_MAP_FILE`로 IOAS에 import. CPU mmap은
시도만 하고 실패해도 진행(cpu_va=NULL).

**확인할 것**
- fd ownership 계약: **성공 시 dmamem이 fd를 소유**, 실패 시 caller
  유지. 실패 시 descriptor를 memset+fd=-1로 되돌려서 #37 memfd
  constructor보다 실패 상태가 깔끔하다 — 두 constructor의 비대칭 자체가
  리뷰 코멘트감.
- [DD #38-6, 조건부] `cpu_va`로 CPU가 읽고 쓸 때 exporter에 따라
  `DMA_BUF_IOCTL_SYNC`가 필요할 수 있는데 API가 표현하지 않는다. 현재
  smoke는 이 cpu_va를 안 쓰므로 향후 소비자 문제.

### 38-2. `418da89` feat(vfio): export a device BAR range as dma-buf (+195)

**들어오는 것**: `vfioctl.h`의 `vfio_device_bar_export_dmabuf()` —
가변 길이 `vfio_device_feature` + `feature_dma_buf` + `dma_range` 1개를
calloc으로 구성, ioctl 반환값이 곧 dma-buf fd. BAR smoketest 동반.

**확인할 것**
- `nr_ranges = 1` 고정 — 당시 커널 구현이 `nr_ranges != 1`을
  `-EOPNOTSUPP`으로 거부하므로 맞는 선택이지만, constructor의 "어떤
  dma-buf든" 문구와 커널의 실제 지원 범위(vfio-pci exporter, 단일
  range)의 간극을 인지하고 읽을 것.
- export한 `offset/length`를 region 크기와 대조하지 않는다. 커널이
  거부해 주는지에 의존.

### 38-3. `ecf3163` feat(dmamem): NVMe DMA into GPU VRAM (+279, 빌드 깨짐)

**들어오는 것**: `dmamem_nvme_vram_smoketest.c` — GPU cdev attach →
BAR export → dmabuf import → NVMe attach(같은 IOAS) → IDENTIFY의
PRP1을 `gpu_bar_iova + 0x2000`으로 → CPU는 BAR mmap으로 확인.

**확인할 것**
- **이 커밋 자체가 컴파일되지 않는다**(open/close 인자 개수 불일치,
  `ad60627`에서 수정). series 정리 요청감.
- `IDENTIFY_OFFSET 0x2000`, 이후 `0x4000`이 BAR/export range 안인지
  검증 없음. BAR index/size는 CLI 입력을 그대로 신뢰.
- [신규, Low] `open_gpu_and_import_bar()`에서 실패 시
  `fprintf(...) 다음에 err = -errno`(vram smoketest 73-86행) —
  fprintf가 errno를 바꿀 수 있어 틀린 에러코드가 전파될 수 있다.
  #37 controller는 `err = -errno`를 먼저 한다. 순서 통일 필요.
- 검증 문구 주의: 성공해도 "선택한 PCI region으로 peer DMA가 됐다"까지.
  그 region이 VRAM인지는 실행 환경의 전제다.

### 38-4. `9b105d0` feat(nvme): IO queue pair on the dmamem path (+147, 빌드 깨짐 지속)

**들어오는 것**: `nvme_admin_sync_dmamem()`(수동 CID 동기 admin),
`nvme_controller_create_io_qpair_dmamem()`(CID 2/3, rollback 시 CID 4로
DELETE CQ), `nvme_controller_delete_io_qpair_dmamem()`(CID 5/6).

**확인할 것** — 이 커밋이 #38의 High 셋 전부다.
- [DD #38-3, High] `nvme_admin_sync_dmamem()`은 첫 phase-valid CQE를
  status만 보고 소비한다. `cpl.cid == cmd.cid` 대조 없음 → timeout 뒤
  늦은 CQE가 다음 명령의 완료로 오귀속.
- [DD #38-1, High] DELETE SQ/CQ가 실패/timeout해도
  `nvme_qpair_dmamem_term()` + `nvme_qid_free()`가 무조건 실행.
  CREATE SQ 실패 rollback은 DELETE CQ 결과를 `(void)`로 버린다.
  장치가 queue를 아직 쓰고 있으면 재할당 메모리에 DMA.
- [DD #38-2, High] `cdw10 = (depth-1) << 16` — depth 0이면 0xFFFF,
  depth > 1024면 64 KiB SQ 초과. 검증 없음.

### 38-5. `ad60627` feat(dmamem): real disk READ into VRAM (+96, 빌드 복구)

**들어오는 것**: qid 1, depth 32 qpair로 NVMe READ(LBA 0, 8블록)를
`bar_iova + 0x4000`으로. 첫 16바이트 fingerprint 출력. **앞 커밋의
컴파일 오류 수정이 여기 섞여 있다.**

**확인할 것**
- [DD #38-5] NSID=1, 512B LBA 고정 가정 — 4 KiB LBA 장치면 32 KiB
  전송인데 PRP1 하나뿐.
- [DD #38-4] all-zero면 "inconclusive" 메시지를 내지만 **exit 0** —
  CI에 물리면 위양성.
- 빌드 수정과 기능 추가가 한 커밋에 섞임 — split 요청감.

---

## PR #39: type1/iommufd 공통화 (2 커밋, +272/−198)

### 39-1. `72c2a4c` feat(driver): unify vfio-pci path (+94/−32)

**들어오는 것**: `upcie_nvme_driver.c`에서 `UPCIE_BACKEND` 제거,
`UPCIE_VFIO_MODE=auto|type1|iommufd` 도입. AUTO는
`#ifdef IOMMU_IOAS_MAP_FILE` + `access("/dev/iommu", R_OK|W_OK)`만 본다.

**확인할 것**
- [DD #39-1, Med] AUTO는 **선택만 하고 fallback하지 않는다**. cdev 부재,
  구 커널의 MAP_FILE 미지원, 권한 오류에서 type1이 가능해도 그대로
  실패. 반대로 구식 헤더로 빌드하면 새 커널에서도 항상 type1
  (빌드 호스트 UAPI 종속).
- [DD #39-2, Med] 기본 동작 변경: `/dev/iommu`가 열리면 기본이 실험적
  iommufd 경로가 된다. 기존 `UPCIE_BACKEND=dmamem` 스크립트는 조용히
  무시됨 — deprecation 경고나 release note 필요.
- [DD #39-3, Med] postcondition 비대칭: type1은 I/O qpair까지 만들고
  iommufd는 IDENTIFY 후 반환 → `nvme->ioq` 사용 가능 여부가 mode에
  따라 달라진다.
- `return -err` 부호 반전이 이 커밋에도 그대로 승계된다.

### 39-2. `599d267` refactor(nvme): shared vfio-pci bring-up helpers (+178/−166)

**들어오는 것**: `nvme_controller_vfio_pci.h` 신설 — `bus_master_enable`,
`acquire_bar0`, `reset_via_bar0`, `enable_via_bar0` 4개를 두 backend에서
추출. 추출 경계는 "device fd를 얻은 뒤 ~ CSTS.RDY까지".

**확인할 것**
- 추출 자체는 라인 단위로 충실하다(diff를 old/new 나란히 보면 동작
  변화 없음). 경계 선택도 적절 — fd 획득/DMA map/heap은 backend에 남김.
- [신규, Low — 리팩토링 회귀] `nvme_controller_vfio_pci.h:88-90, 94-96`:
  원래 코드는 `err = -errno`를 **UPCIE_DEBUG 호출 전에** 캡처했는데,
  추출된 helper는 `UPCIE_DEBUG(...)` **뒤에** `return -errno`를 한다.
  디버그 빌드에서 fprintf가 errno를 바꾸면 틀린 코드 반환.
- [DD #39-5, High] enable timeout 후 quiesce 없이 메모리 반환하는 #37의
  문제가 공통 helper로 **공유**됐을 뿐 해결되지 않았다. helper가
  timeout 시 controller 상태를 알려주거나 caller가 detach 후 free해야.

---

## PR #40: translator + host/CUDA/HIP/UIO (8 커밋, +1379/−93)

> 주의: `9026d9d`~`319854b` 세 커밋은 빌드는 되지만 **기존 VRAM smoke가
> 런타임에서 반드시 실패**한다(4 MiB PRP scratch > 2 MiB admin heap).
> `be72922`가 heap을 16 MiB로 키워서야 복구. 기능 bisect 불가 구간.

### 40-1. `9026d9d` feat(nvme): rpool + per-request PRP scratch (+121/−23)

**들어오는 것**: `nvme_qpair_dmamem_init()`이 request pool(calloc)과
1024×4 KiB PRP scratch를 같은 dmamem_heap에서 확보.
`nvme_request_pool_init_prps_dmamem()` 신설. term/create/delete에
`prp_offset` 파라미터 추가.

**확인할 것**
- [DD #40-4, High] `nvme_request.h`의 pool init: base offset을
  `dmamem_heap_at_iova()`로 **한 번만** 변환한 뒤
  `reqs[i].prp_addr = prps_iova + i*4096`. LUT heap에서 4 MiB scratch는
  2 MiB hugepage 경계를 반드시 넘고, 경계 뒤 PA 연속성 보장이 없다 →
  두 번째 hugepage부터 틀린 PRP-list 주소. 각 4 KiB를
  `dmamem_offset_to_iova()`로 개별 변환하거나 scratch를 ARITHMETIC
  heap에 강제해야 한다.
- 이 커밋 시점의 VRAM smoke admin heap은 2 MiB → 할당 실패로 런타임
  회귀(위 박스). heap 확장(be72922)과 같은 커밋이었어야 한다.
- [신규, Low] `calloc(rpool)` 실패 시 `return -errno` — calloc이 errno를
  설정한다는 보장은 POSIX 관례 수준. `-ENOMEM` 직접 반환이 안전.

### 40-2. `284f65b` feat(dmamem): discriminated translator + destroy dispatch (+295/−46)

**들어오는 것**: 이 PR의 중심 커밋. `struct dmamem`에
`translator`(ARITHMETIC/LUT), `phys_lut/hugepgsz/hugepgsz_shift`,
`vfio_container`, `owned` 추가. `dmamem_destroy()`가 iommufd/type1/무
mapping을 dispatch. `dmamem_heap_at_iova()`가 `dmamem_offset_to_iova()`
경유로 변경. PRP builder 2개
(`nvme_request_prep_command_prps_{contig,iov}_dmamem`) 추가.

**읽을 위치**
- `dmamem.h:176-198` fast path와 `dmamem_lut_pagesize_shift()`(4K/2M/1G만)
- `nvme_request.h`의 contig builder LUT 분기(hugepage 경계 재변환 로직)
- `nvme_request.h`의 iov builder

**확인할 것**
- [정정/세분화 — 신규] **contig builder는 LUT를 올바르게 다룬다**:
  `strides_left`로 hugepage 안에서는 stride, 경계에서
  `dmamem_va_to_iova()` 재변환. deep dive의 "LUT 불연속 무시"는
  40-1의 pool scratch에만 해당한다. 코드를 볼 때 이 둘을 혼동하지 말 것.
  단 contig builder도 `assert(npages <= 513)`뿐이라 NDEBUG에서 512-entry
  PRP page를 넘는 입력을 막지 못한다.
- [DD #40-5, High] iov builder는 `dvec_cnt` 확인 **전에** `dvec[0]`을
  읽고, `prp_idx` 상한이 없어 512 entry를 넘으면 request scratch 뒤
  host memory를 덮어쓴다. 반환형이 void라 오류 전달 경로도 없다.
- [DD #40-10, High] `dmamem_offset_to_iova()`는 `offset < size`, LUT
  index 범위, `base_iova + offset` overflow를 전혀 검사하지 않고,
  descriptor에 LUT entry 수 필드 자체가 없어 constructor도 검증할 수
  없다. `lut_entries`를 추가하고 checked/unchecked를 분리해야 한다.
- owned 도입이 기존 dmabuf constructor의 "destroy가 fd를 닫아야 하는데
  owned 개념이 없던" 문제를 소급 수정 — 커밋 메시지가 스스로
  pre-existing bug fix라고 밝히는 부분. #38에 넣었어야 할 수정이다.

### 40-3. `319854b` refactor(hostmem): move phys_lut onto hostmem_hugepage (+54/−14)

**들어오는 것**: LUT 소유권을 `hostmem_heap` → `hostmem_hugepage`로
이동. pagemap 읽기가 "best-effort"가 되어 `-EPERM`이면 phys/phys_lut를
0/NULL로 두고 성공 처리.

**확인할 것**
- [DD #40-3, High] **best-effort 분기가 실효가 없다**:
  `hostmem_pagemap_virt_to_phys()`(hostmem.h)는 present bit만 보고
  PFN 0을 유효값으로 반환한다. 현대 커널은 CAP_SYS_ADMIN 없을 때
  EPERM 대신 **PFN을 0으로 마스킹**하므로, err==0인 채 0으로 가득 찬
  LUT가 만들어진다. `hostmem_heap_init`의
  `memory.phys != phys_lut[0]` 검사도 0==0이라 통과. UIO 경로에서 PA 0
  근처로 DMA할 수 있다. PFN==0 거부 + capability 선검사가 필요.
- [신규, Med — 설계 목표 미달] 커밋 메시지는 "arithmetic 소비자는 root
  요구를 벗는다"고 하지만 `hostmem_heap_init()`은 여전히
  `phys_lut == NULL`이면 `-EPERM`으로 실패한다. best-effort 혜택은
  `hostmem_hugepage`를 직접 쓰는 쪽만 받고, heap 경유 소비자(type1
  driver 경로 포함)는 그대로 root가 필요하다.
- 소유권 이동 자체(borrow 포인터 정리, free 위치)는 깔끔하다.
- [DD #40-13, Low] `hostmem_hugepage_import()`는 nphys/phys_lut를
  채우지 않아 alloc 경로와 비대칭.

### 40-4. `be72922` feat(dmamem): wrapping constructors from hostmem_hugepage (+174/−3)

**들어오는 것**: `dmamem_hostmem.h`의 constructor 3개
(`_iommufd`: IOAS_MAP, kernel-picked IOVA / `_type1`: MAP_DMA,
caller-picked IOVA / `_lut`: ioctl 없음, LUT borrow). 모두 `owned=0`.
VRAM smoke의 admin heap을 16 MiB로 확대(40-1 회귀 복구).

**확인할 것**
- 세 constructor 모두 실패 시 memset+fd=-1로 되돌린다 — #37 memfd
  constructor와의 비대칭이 더 두드러진다(memfd만 destroy-unsafe).
- `_type1`은 base_iova 충돌/aperture 검증이 전부 caller 책임 — 문서로
  명시할 것.
- [DD #40-12, Med] borrow에 refcount가 없다: source hugepage나
  iommufd/container가 wrapper보다 먼저 죽으면 dangling. destroy의
  unmap 실패는 로그만 남기고 descriptor를 지워 재시도 불가.
- runtime 회귀 복구(heap 16 MiB)가 이 커밋에 섞여 있다 — 40-1로
  옮겨야 bisect가 산다.

### 40-5. `aa65bd2` feat(example): LUT smoketest (+174)

**들어오는 것**: `dmamem_hostmem_lut_smoketest.c` — hugepage base,
내부 offset, 마지막 바이트를 `dmamem_va_to_iova()`로 변환해
`phys_lut[i] + in_hp_offset`과 비교.

**확인할 것**
- **순환 검증**: expected가 같은 `hp->phys_lut`에서 나온다. 40-3의
  zero-LUT 결함이 있어도 0==0으로 통과한다. 독립 소스(예: 실제 device
  DMA, 또는 최소한 PFN!=0 assert)가 필요.
- YAML 설명은 sudo를 언급하는데 실제 command에는 없다.

### 40-6. `b78b953` feat(dmamem): from_cuda_lut + from_hip_lut (+163/−19)

**들어오는 것**: `dmamem_cuda.h`/`dmamem_hip.h` — 기존
cudamem/hipmem_heap의 phys_lut를 borrow하는 얇은 wrapper. `cpu_va=NULL`.
`dmamem_lut_pagesize_shift()`로 helper 통합.

**확인할 것**
- [DD #40-2, High] **기본 설정으로 항상 실패**:
  `cudamem_config_init()`은 `device_pagesize = 65536`(cudamem_config.h:118)
  인데 `dmamem_lut_pagesize_shift()`는 4K/2M/1G만 허용 →
  `dmamem_from_cuda_lut()`가 기본 heap에 `-EINVAL`. HIP은 4 KiB라 통과.
  64 KiB를 허용 목록에 넣거나 generic power-of-two + ctz로 바꿔야 한다.
- [DD #40-9, Med] `cpu_va=NULL`이므로 40-2의 generic PRP builder
  (`dmamem_va_to_iova` 기반)에 GPU buffer를 넣을 수 없다(assert).
  offset 기반 builder가 없어서 end-to-end NVMe 경로가 API로 완결되지
  않는다 — 실제로 CUDA→UIO NVMe example이 이 PR에 없다.
- 이 경로는 IOMMU 우회(no-IOMMU)다. #38의 IOMMUFD CUDA 경로 대체가
  아니라는 점을 문서/커밋 문구에서 유지하는지 확인
  (관계 문서 §4.4, §6 참조).

### 40-7. `13010ae` feat(nvme): dmamem controllers for uio + type1 (+410)

**들어오는 것**: `nvme_controller_dmamem_uio.h`(pci_func_open +
pci_bar_map으로 BAR0, 나머지는 공통 helper 합류),
`nvme_controller_dmamem_type1.h`(caller-owned container/group에서
device fd). IO qpair/admin helper는 vfio 변형을 그대로 재사용.

**확인할 것**
- [DD #40-1, High] **UIO 경로에 Bus Master Enable이 없다**:
  `nvme_controller_open_dmamem_uio()`는 BAR mmap만 한다.
  `uio_pci_generic`도 probe에서 `pci_set_master()`를 하지 않는다.
  외부에서 BME가 켜져 있지 않으면 admin 명령이 전부 timeout.
  VFIO 형제들은 공통 helper에서 명시적으로 켠다 — 비대칭.
- [DD #40-7, High] translator/domain 일치 검증 부재: UIO 주석은 "LUT
  heap이어야 한다"고 하지만 `heap->dmem->translator`를 검사하지 않고,
  type1/iommufd controller도 heap의 mapping이 자기 container/IOAS에
  속하는지 확인하지 않는다. 잘못된 조합이 조용히 AQA/ASQ/ACQ에
  기록된다. open 시 `-EXDEV`류 거부 필요.
- [DD #40-6, High] enable timeout/RDY=0 실패/DELETE 실패 후에도
  SQ/CQ/PRP를 heap에 반환하는 패턴이 세 controller 전부에 복제됐다.
- [DD #40-11] PC=1 큐가 LUT hugepage 경계를 넘는 배치 가능성 — 4 KiB
  정렬만 보장하는 allocator에서 64 KiB 큐가 경계를 걸치면 물리 불연속.

### 40-8. `a4f9a4f` ver: bump to v0.5.2 (+9/−9)

버전 표기만. 기능 변경 없음. 스택 전체에 `meson test` 등록 테스트가
0개라는 점만 재확인.

---

## 커밋을 관통하는 체크리스트

코드를 읽는 동안 반복해서 물어볼 질문 (deep dive §7과 동일 축):

| 축 | 질문 | 주로 걸리는 커밋 |
|---|---|---|
| 소유권/종료 | 실패한 객체에 destroy를 불러도 되는가? free 전에 장치 DMA가 차단됐는가? | 26d1646, 5a18811, 9b105d0, 13010ae |
| 주소 변환 | offset/길이/overflow를 누가 검증하는가? LUT 경계에서 재변환하는가? | 284f65b, 9026d9d |
| 백엔드 대칭성 | type1/iommufd/UIO가 같은 postcondition, 같은 BME 보장을 갖는가? | 72c2a4c, 13010ae |
| 검증 독립성 | 테스트의 expected가 검증 대상과 같은 소스에서 나오지 않는가? | 8fe3663, aa65bd2, ad60627 |
| bisect | 커밋 단독으로 빌드/동작하는가? | ecf3163, 9b105d0, 9026d9d~319854b |

## 다음 단계

이 가이드로 코드를 직접 본 뒤, 확정한 이슈를 골라 리뷰 문서(또는 GitHub
코멘트)로 정리한다. 우선순위 제안은 deep dive §9와 동일하게 **#37 heap
overflow부터** — 입력만으로 재현되고 뒤의 세 PR 전부가 이 heap 위에
서 있기 때문이다.

## 관련 문서

- [IOMMUFD upstream patches and Simon's PRs](./IOMMUFD_UPSTREAM_SIMON_PRS_RELATION.md)
- [Simon PR #37~#40 deep dive](./iommufd_prs_deep_dive.md)
- [Simon PR #37~#39 분석 보고서](./iommufd_prs_analysis.md)
