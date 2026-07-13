# upcie PR #37~#40 쉽게 읽는 분석

이 네 PR의 목적은 **NVMe가 host memory와 GPU memory를 같은 방식으로
DMA하도록 주소 변환과 DMA mapping 정리 규칙을 `dmamem` API 뒤에 모으는
것**이다. 실제 backing의 소유권과 수명은 owned constructor와 borrowed
wrapper에 따라 다르다.

이 문서는 PR 네 개를 처음 보는 리뷰어를 위한 안내서다. 세부 ioctl을 모두
외우기보다 다음 세 가지를 이해하는 것이 목표다.

1. 각 PR이 앞 PR 위에 무엇을 추가하는가?
2. NVMe가 실제로 사용하는 DMA 주소는 어떻게 만들어지는가?
3. merge 전에 반드시 확인해야 할 오류는 무엇인가?

PR은 서로 독립적이지 않고 아래 순서로 쌓여 있다.

```text
main
  └─ #37  iommufd와 dmamem 기반
       └─ #38  dma-buf와 GPU BAR 실험
            └─ #39  VFIO 경로 통합
                 └─ #40  host/CUDA/HIP와 UIO/type1까지 확장
```

따라서 리뷰할 때도 각 PR을 바로 앞 PR과 비교해야 한다.

```bash
git diff origin/main..origin/pr/37
git diff origin/pr/37..origin/pr/38
git diff origin/pr/38..origin/pr/39
git diff origin/pr/39..origin/pr/40
```

| PR | 역할 | 가장 먼저 볼 위험 |
|---|---|---|
| [#37](https://github.com/safl/upcie/pull/37) | iommufd와 dmamem 기반 | allocator overflow와 실패 cleanup |
| [#38](https://github.com/safl/upcie/pull/38) | dma-buf/GPU BAR PoC | queue 삭제 실패 뒤 메모리 재사용 |
| [#39](https://github.com/safl/upcie/pull/39) | type1/iommufd 공통화 | AUTO의 runtime fallback 부재 |
| [#40](https://github.com/safl/upcie/pull/40) | LUT, host/CUDA/HIP, UIO 확장 | 주소 변환과 PRP 범위 오류 |

## 분석 기준

각 PR의 GitHub 화면이 아니라 실제 stacked base를 기준으로 분석했다. 따라서
#38에서 보이는 #37 코드나 #40에서 보이는 #37~#39 코드를 해당 PR의 신규
변경으로 잘못 세지 않는다.

| PR | 분석 범위 | 커밋 | diff 규모 |
|---|---|---:|---:|
| #37 | `209b8ea..cbcccc9` | 5 | 16 files, `+1963/-0` |
| #38 | `cbcccc9..ad60627` | 5 | 11 files, `+797/-0` |
| #39 | `ad60627..599d267` | 2 | 8 files, `+272/-198` |
| #40 | `599d267..a4f9a4f` | 8 | 22 files, `+1379/-93` |

문서 안의 평가는 다음 세 종류로 구분한다.

- **구현 사실**: 코드, 커밋 메시지 또는 테스트가 직접 보여주는 동작
- **확정 문제**: 코드 경로를 따라가거나 작은 harness로 재현한 결함
- **리뷰 판단**: 특정 장치 상태나 호출 방식에 의존해 추가 검증이 필요한 위험

네 head는 모두 깨끗한 별도 checkout에서 기본 Meson build가 성공했다. 이는
컴파일 확인일 뿐 실제 NVMe/GPU DMA 성공을 의미하지 않는다. 하드웨어 테스트와
CI 연결 상태는 8장에서 따로 다룬다.

---

## 1. 먼저 알아둘 용어

| 용어 | 쉬운 설명 |
|---|---|
| DMA | CPU가 복사하지 않고 PCIe 장치가 메모리를 직접 읽고 쓰는 방식 |
| PA | 실제 RAM이나 장치 메모리의 물리 주소 |
| IOVA | 장치가 보는 가상 주소. IOMMU가 뒤에서 PA로 바꾼다 |
| IOMMU | 장치의 메모리 접근 범위를 제한하고 IOVA를 PA로 변환하는 장치 |
| IOAS | iommufd가 관리하는 하나의 I/O 주소 공간 |
| dma-buf | 서로 다른 드라이버가 같은 메모리를 공유할 때 쓰는 fd 기반 규약 |
| BAR | PCIe 장치가 CPU에 공개하는 메모리 창 |
| PRP | NVMe 명령에 들어가는 데이터 버퍼의 DMA 주소 |
| SQ/CQ | NVMe 명령을 넣는 큐와 완료 결과를 받는 큐 |
| VFIO type1 | 기존 `/dev/vfio/<group>` 기반 IOMMU 사용 방식 |
| iommufd | 새 `/dev/iommu`와 VFIO device cdev를 사용하는 방식 |
| UIO | 이 PR의 UIO 경로는 `iommu=pt/off`를 전제로 PA를 직접 사용한다 |

이 문서에서 가장 중요한 구분은 PA와 IOVA다.

```text
#40의 UIO 경로(iommu=pt/off 전제)
  NVMe가 PA를 직접 사용
  페이지가 물리적으로 흩어져 있으면 페이지마다 주소를 찾아야 함

VFIO type1 또는 iommufd
  NVMe가 IOVA를 사용
  IOMMU가 흩어진 물리 페이지를 연속된 IOVA 창으로 보이게 만들 수 있음
```

---

## 2. 네 PR이 만드는 전체 구조

최종 #40 기준 데이터 흐름은 다음과 같다.

```text
메모리 원본
  memfd / host hugepage / VFIO BAR / CUDA / HIP
                       │
                       ▼
                    dmamem
        "offset을 DMA 주소로 바꾸는 공통 규칙"
                       │
          ┌────────────┴────────────┐
          ▼                         ▼
 CPU-mappable queue dmamem      data dmamem
          │                  host / BAR / CUDA / HIP
          ▼                         │
     dmamem_heap                    │
   SQ/CQ와 PRP scratch              │
          └────────────┬────────────┘
                       ▼
             NVMe PRP와 queue 주소
                       │
                       ▼
       iommufd / VFIO type1 / UIO controller
```

현재 controller API는 queue용과 data용 dmamem을 타입으로 분리하지 않는다.
하지만 CUDA/HIP처럼 `cpu_va == NULL`인 backing은 SQ/CQ와 PRP list를 CPU가
채울 수 없으므로 data 주소로만 써야 한다. 이 계약을 검사하지 않는 것이
#40의 주요 문제 중 하나다.

`dmamem`의 핵심 역할은 `offset -> DMA 주소` 변환이다. 최종 API에는 두
가지 변환 방식이 있다.

### ARITHMETIC: 시작 주소에 offset 더하기

```text
DMA 주소 = base_iova + offset
```

iommufd나 VFIO type1이 메모리 전체를 연속된 IOVA로 매핑했을 때 사용한다.
빠르고 단순하다.

### LUT: 페이지 표에서 PA 찾기

```text
DMA 주소 = phys_lut[페이지 번호] + 페이지 안쪽 offset
```

UIO처럼 장치가 PA를 직접 사용할 때 필요하다. 중요한 점은 LUT의 다음
페이지 주소가 앞 페이지 주소에 페이지 크기를 더한 값이라는 보장이 없다는
것이다. #40의 여러 오류가 이 차이를 놓친 데서 나온다.

### 최종 #40의 조합 행렬

`dmamem`의 주 역할은 메모리를 NVMe가 사용할 주소 체계로 설명하는
descriptor다. memfd owned constructor는 backing까지 직접 만들고, dma-buf와
host/CUDA/HIP 경로는 기존 backing을 가져오거나 감싼다. 최종 코드가
표현하려는 조합은 다음과 같다.

| 메모리 원본 | DMA API | 변환 | CPU VA | 생성자 |
|---|---|---|---|---|
| hugepage memfd | iommufd | ARITHMETIC | 있음 | `dmamem_from_memfd` |
| dma-buf/VFIO BAR | iommufd | ARITHMETIC | exporter 의존 | `dmamem_from_dmabuf` |
| 기존 host hugepage | iommufd | ARITHMETIC | 있음 | `dmamem_from_hostmem_iommufd` |
| 기존 host hugepage | VFIO type1 | ARITHMETIC | 있음 | `dmamem_from_hostmem_type1` |
| 기존 host hugepage | UIO, `iommu=pt/off` | LUT | 있음 | `dmamem_from_hostmem_lut` |
| 기존 CUDA/HIP heap | UIO, `iommu=pt/off` | LUT | 없음 | `dmamem_from_{cuda,hip}_lut` |

여기서 `cpu_va == NULL`은 DMA가 불가능하다는 뜻이 아니다. CPU 포인터로
접근할 수 없으므로 호출자가 GPU allocation 안의 offset을 알고
`dmamem_offset_to_iova()`를 사용해야 한다는 뜻이다.

### 소유권의 두 종류

#40은 constructor를 owned와 wrapping으로 구분한다.

- **owned**: memfd/dma-buf fd와 CPU mmap을 `dmamem`이 소유한다.
  `dmamem_destroy()`가 IOMMU mapping, mmap, fd를 정리한다.
- **wrapping**: host/CUDA/HIP allocator는 호출자가 계속 소유한다.
  `dmamem`은 mapping 또는 LUT를 빌리며, destroy는 자신이 설치한 mapping만
  제거한다. LUT wrapper라면 제거할 mapping도 없다.

따라서 올바른 종료 순서는 항상 controller와 qpair를 먼저 멈추고, 그 뒤
`dmamem_heap`, `dmamem`, 실제 backing allocator 순서로 내려가야 한다. 이
순서가 뒤집히면 장치가 해제된 주소로 DMA할 수 있다.

---

## 3. PR #37: 기반 만들기

### 한 줄 요약

`iommufd`로 메모리와 NVMe 장치를 같은 IOAS에 연결하고, 그 위에서 사용할
`dmamem`과 작은 heap allocator를 추가한다.

### 정상 흐름

```text
open("/dev/iommu")
  -> IOAS 생성
  -> hugepage memfd 생성 및 mmap
  -> memfd를 IOAS에 매핑하고 base_iova 받기
  -> NVMe VFIO device cdev를 같은 IOAS에 attach
  -> dmamem_heap에서 Admin SQ/CQ 할당
  -> ASQ/ACQ에 IOVA 기록
  -> controller enable
```

### 커밋별 구성

| 순서 | 커밋 | 핵심 변경 | 다음 커밋이 의존하는 것 |
|---:|---|---|---|
| 1 | [`9beffae`](https://github.com/safl/upcie/commit/9beffae) | iommufd와 VFIO device cdev ioctl wrapper | IOAS와 device attach API |
| 2 | [`26d1646`](https://github.com/safl/upcie/commit/26d1646) | `dmamem`, memfd constructor, offset heap | 공통 DMA 주소와 allocator |
| 3 | [`8fe3663`](https://github.com/safl/upcie/commit/8fe3663) | memfd smoke task와 deploy 진단 | 실행 환경 확인 |
| 4 | [`5a18811`](https://github.com/safl/upcie/commit/5a18811) | dmamem 기반 NVMe admin queue와 Identify | 실제 device consumer |
| 5 | [`cbcccc9`](https://github.com/safl/upcie/commit/cbcccc9) | `upcie_nvme_driver`의 dmamem backend | smoke test 밖의 사용 예 |

첫 두 커밋은 메모리 기반, 네 번째는 장치 기반, 마지막은 application 통합이다.
이 순서 때문에 allocator나 lifetime 오류는 이후 #38~#40 전체로 전파된다.

### `iommufd.h`: kernel object를 얇게 감싸는 계층

[`iommufd.h`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/include/upcie/iommufd.h#L55-L460)는
두 개의 userspace handle만 정의한다.

```c
struct iommufd {
    int fd;                 // /dev/iommu
};

struct iommufd_device {
    int fd;                 // /dev/vfio/devices/vfioN
    uint32_t devid;         // BIND가 반환한 kernel object id
};
```

wrapper와 kernel UAPI의 대응은 다음과 같다.

| userspace 함수 | 동작/UAPI | 결과와 소유권 |
|---|---|---|
| `iommufd_open/close` | `/dev/iommu` open/close | close 시 그 fd가 소유한 IOAS와 mapping도 제거 |
| `iommufd_ioas_alloc` | `IOMMU_IOAS_ALLOC` | `ioas_id` 반환 |
| `iommufd_destroy` | `IOMMU_DESTROY` | ID로 IOAS 같은 객체 제거 |
| `iommufd_ioas_map` | `IOMMU_IOAS_MAP` | user VA를 IOVA에 map |
| `iommufd_ioas_map_file` | `IOMMU_IOAS_MAP_FILE` | memfd/dma-buf range를 IOVA에 map |
| `iommufd_ioas_unmap` | `IOMMU_IOAS_UNMAP` | IOVA와 길이로 mapping 제거 |
| `iommufd_device_open` | VFIO device cdev open | device fd를 userspace가 소유 |
| `iommufd_device_bind` | `VFIO_DEVICE_BIND_IOMMUFD` | device를 iommufd에 bind하고 `devid` 저장 |
| `iommufd_device_attach` | `VFIO_DEVICE_ATTACH_IOMMUFD_PT` | device를 지정 IOAS에 attach |
| `iommufd_device_detach` | `VFIO_DEVICE_DETACH_IOMMUFD_PT` | 명시적 detach |

모든 ioctl 구조체를 0으로 초기화하고 `size` 또는 `argsz`를 채우며, 실패는
`-errno`로 올린다. `IOMMU_IOAS_MAP_FIXED_IOVA`를 주지 않으면 map 함수의
`iova`는 출력값이고 kernel이 위치를 고른다.

호환성은 두 단계다. `<linux/iommufd.h>`나 VFIO bind UAPI가 없으면 관련
함수는 `-ENOTSUP` stub으로 컴파일되고, `IOMMU_IOAS_MAP_FILE`만 없는 헤더도
별도 stub을 사용한다. 주석은 caller가 `IOMMU_IOAS_MAP`으로 fallback할 수
있다고 설명하지만, #37의 `dmamem_from_memfd()` 자체는 fallback하지 않고
`-ENOTSUP`을 그대로 반환한다. 즉 컴파일 호환성은 있지만 기능 호환성이
자동으로 생기는 것은 아니다.

### 초기 `dmamem`: 연속 IOVA 한 개를 표현

[`dmamem.h`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/include/upcie/dmamem.h#L54-L155)의
#37 버전은 아직 ARITHMETIC/LUT 구분이 없다. 모든 backing을 다음 튜플로
표현한다.

| 필드 | 의미 | 소유권 |
|---|---|---|
| `fd` | memfd 또는 dma-buf fd | `dmamem` 소유 |
| `cpu_va` | CPU mmap, 불가능하면 `NULL` | 있으면 `dmamem` 소유 |
| `size` | 전체 byte 길이 | 값 |
| `base_iova` | IOAS 안의 연속 창 시작 | mapping의 결과 |
| `ioas_id` | mapping이 속한 IOAS | 값 |
| `iommufd` | mapping을 설치한 handle | 호출자에게서 빌림 |
| `backing` | MEMFD/DMABUF 구분 | 값 |

제출 경로는 `base_iova + offset` 한 번으로 끝난다. CPU 포인터가 있으면
`base_iova + (vaddr - cpu_va)`로 같은 주소를 얻는다. 두 함수 모두 offset,
VA 범위와 덧셈 overflow를 검사하지 않으므로 public API라면 precondition을
문서화하거나 checked variant가 필요하다.

`dmamem_destroy()`는 `IOAS_UNMAP -> munmap -> close(fd)` 순서로 정리하고
caller-owned iommufd는 닫지 않는다. 따라서 iommufd와 IOAS는 모든 dmamem보다
오래 살아야 한다.

### `dmamem_from_memfd()`: acquire와 rollback

[`dmamem_memfd.h`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/include/upcie/dmamem_memfd.h#L39-L126)의
성공 경로는 다음과 같다.

```text
입력과 hugepage size 확인
  -> memfd_create(MFD_HUGETLB | MFD_HUGE_2MB/1GB)
  -> ftruncate(size)
  -> mmap(PROT_READ|PROT_WRITE, MAP_SHARED)
  -> mlock(size)
  -> memset 전체 범위                         # page fault/pin 유도
  -> IOMMU_IOAS_MAP_FILE(READABLE|WRITEABLE)
  -> kernel-picked base_iova 저장
```

hugepage pool과 충분한 `RLIMIT_MEMLOCK`/권한이 외부 선행조건이다. 실패 시
CPU mapping을 `munmap()`하고 fd를 닫는다. 하지만 IOAS mapping 전에
`iommufd`, `ioas_id`, `size`를 descriptor에 기록하므로 실패한 descriptor를
일반 destroy API에 넘길 수 있는지는 별도 계약 문제가 된다.

### `dmamem_heap`: offset-space first-fit allocator

[`dmamem_heap.h`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/include/upcie/dmamem_heap.h#L20-L258)는
allocation metadata를 backing 안이 아니라 host `calloc()` 객체로 보관한다.
그래서 backing이 CPU-mappable하지 않아도 allocator 자체는 동작한다.

```text
초기 freelist: [ offset 0, size dmem->size, free ]

allocate(size, alignment):
  첫 번째 free block 탐색
  -> aligned offset 앞의 gap을 free block으로 분리
  -> 요청 뒤의 tail을 free block으로 분리
  -> 가운데 block을 allocated로 표시

free(offset):
  exact offset block 탐색
  -> free 표시
  -> 물리적으로 인접한 next/previous free block과 병합
```

API가 pointer 대신 offset을 반환하는 것이 핵심이다. `heap_at_va()`는 CPU
mapping이 없으면 `NULL`을 반환하고, `heap_at_iova()`는 항상 DMA 주소를
반환한다. heap은 `dmamem`을 빌릴 뿐이므로 `heap_term()`이 backing이나
IOMMU mapping을 제거하지 않는다.

### NVMe controller: device attach에서 RDY까지

[`nvme_controller_dmamem_vfio.h`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/include/upcie/nvme/nvme_controller_dmamem_vfio.h#L30-L395)는
기존 `nvme_qpair`의 enqueue, doorbell, reap 로직을 재사용하고 queue backing만
`dmamem_heap`에서 얻는다.

open 단계는 다음과 같다.

1. VFIO device cdev open, iommufd bind, IOAS attach
2. VFIO config region을 통해 `PCI_COMMAND_MASTER` 설정
3. BAR0 정보 조회와 mmap, `ctrlr->func.bars[0]` 구성
4. `CC.EN=0` 기록 후 `CSTS.RDY=0` 대기
5. Admin SQ/CQ 각각 64 KiB를 4 KiB 정렬로 heap에서 할당
6. `AQA`, `ASQ`, `ACQ`에 depth와 IOVA 기록
7. CSS/MPS/SQES/CQES와 `CC.EN=1` 기록 후 `CSTS.RDY=1` 대기

`nvme_dmamem_vfio_ctx`는 device fd, attach 여부, BAR mmap, Admin SQ/CQ
offset을 보관한다. 정상 close는 controller disable과 RDY=0 대기를 먼저 하고
queue memory를 반환한 뒤 BAR unmap, device detach/close를 수행한다.

#37의 qpair에는 request pool(`rpool`)과 per-request PRP scratch가 없다. 그래서
smoke test와 driver는 generic request API 대신 고정 CID를 넣은 command를 직접
enqueue/reap한다. 기존 `nvme_qpair_submit_sync()`를 같은 qpair에 사용하면
request pool을 기대하므로 안전하지 않으며, 이 계약 차이는 #40 첫 커밋에서
보완된다.

### driver 통합과 실제 지원 범위

`upcie_nvme_driver.c`는 `NVME_BACKEND_DMAMEM`과 여러 `*_alive` flag를 추가한다.
정상 생성 순서는 iommufd, IOAS, dmamem, heap, controller, Identify buffer이고
cleanup은 정확히 역순이다. sysfs에서 `<BDF>/vfio-dev/vfioN`을 찾아 device
cdev path도 직접 만든다.

이 backend는 Identify Controller까지만 수행한다. 당시 generic I/O qpair API가
`hostmem_heap`을 요구하므로 실제 READ/WRITE queue 생성은 명시적으로 범위 밖이다.
#38이 dmamem 전용 I/O qpair helper를 추가해 이 제한을 다음 단계에서 푼다.

### #37 자원 수명 표

| 자원 | 생성/획득 | 소유자 | 해제 |
|---|---|---|---|
| `/dev/iommu` fd | `iommufd_open` | application | `iommufd_close` |
| IOAS | `iommufd_ioas_alloc` | application | `iommufd_destroy` |
| memfd와 CPU mmap | `dmamem_from_memfd` | `dmamem` | `dmamem_destroy` |
| IOAS memory mapping | `MAP_FILE` | `dmamem` | `dmamem_destroy` |
| heap metadata | `dmamem_heap_init` | `dmamem_heap` | `dmamem_heap_term` |
| VFIO device fd/BAR/attach | controller open | controller ctx | controller close |
| Admin SQ/CQ allocation | qpair init | controller ctx | qpair term |

정상 종료의 핵심 불변식은 `controller < heap < dmamem < IOAS < iommufd` 순으로
먼저 왼쪽을 해제하는 것이다.

### 먼저 고쳐야 할 부분

1. **High: allocator 크기 계산 overflow**

   `front_gap + size`가 `size_t` 범위를 넘으면 작은 빈 공간에도 매우 큰
   요청이 들어간 것으로 판단한다. 실제로 16바이트 heap에서 `SIZE_MAX`
   요청이 성공하고 서로 겹치는 free block이 만들어진다.

   검사는 덧셈보다 뺄셈을 사용해야 한다.

   ```c
   if (front_gap > block_size || size > block_size - front_gap)
       continue;
   ```

2. **Medium: 임의 alignment를 받지만 계산은 2의 거듭제곱 전용**

   현재 bitmask 정렬식은 1, 2, 4, 8 같은 값에서만 맞다. `alignment=3`을
   주면 3의 배수가 아닌 offset을 반환한다. API가 2의 거듭제곱만 받도록
   검사해야 한다.

3. **High: `hugepgsz=0`이면 오류 반환 대신 SIGFPE**

   [`dmamem_from_memfd()`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/include/upcie/dmamem_memfd.h#L46-L61)는
   0인지 확인하기 전에 `size % hugepgsz`를 계산한다. public constructor에 0을
   주면 `-EINVAL`이 아니라 process가 SIGFPE로 종료된다. 지원하는 page
   size인지 먼저 검사해야 한다.

4. **Medium: 생성에 실패한 dmamem의 상태가 destroy-safe하지 않음**

   실제 IOAS 매핑 전에 `size`와 iommufd 정보를 기록한다. 생성 도중 실패한
   객체에 `dmamem_destroy()`를 호출하면 매핑하지 않은 IOVA 0을 unmap하려고
   할 수 있다. 현재 API는 실패한 객체를 destroy해도 되는지 명확히 말하지
   않는다. 실패 후 상태를 초기화하거나, `mapped` 상태를 두거나, cleanup
   계약을 문서화해야 한다.

5. **High: controller enable 실패 뒤 메모리를 너무 빨리 반환**

   `CC.EN=1` 이후 RDY timeout이 나도 명시적으로 disable하지 않고 Admin
   SQ/CQ를 heap에 돌려준다. 장치가 늦게 DMA하면 재사용된 메모리를 건드릴
   수 있다.

6. **Medium: 공개 qpair initializer가 depth를 검증하지 않음**

   [`nvme_qpair_dmamem_init()`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/include/upcie/nvme/nvme_controller_dmamem_vfio.h#L145-L186)은
   depth와 관계없이 SQ/CQ를 각각 64 KiB만 할당한다. SQ entry는 64바이트이므로
   depth 1024까지만 이 범위에 들어간다. 1025 이상이면 enqueue가 다음 heap
   영역을 덮을 수 있고, depth 0이면 `(tail + 1) % depth`에서 0으로 나눈다.
   controller의 `CAP.MQES`와 비교하는 검사도 없다.

   #37의 내부 caller는 depth 256을 고정해 이 결함을 바로 밟지는 않는다.
   그러나 이 커밋이 설치하는 public inline API의 입력 계약은 이미 깨져 있고,
   #38이 caller 지정 I/O queue depth를 사용하면서 영향 범위가 넓어진다.

7. **API 제약: Admin qpair에 request pool이 없음**

   #37의 initializer는 `struct nvme_qpair`를 0으로 만든 뒤 SQ/CQ만 채우므로
   `qp->rpool == NULL`이다. 직접 `nvme_qpair_enqueue()`와 reap을 사용하는 것은
   가능하지만, 기존 `nvme_qpair_submit_sync*()` 계열은 request pool을
   역참조한다. 두 example은 CID 1을 직접 지정해 이 차이를 피한다.

   **구현 사실**은 같은 `struct nvme_controller`를 반환하면서 generic admin
   submission API 일부를 사용할 수 없다는 것이다. **리뷰 판단**으로는 별도
   controller type을 쓰거나, unsupported API를 명시하거나, open 시 request
   pool까지 완성해야 한다. #40의 첫 커밋이 세 번째 방식을 선택한다.

8. **통합 문제: dmamem 모드도 128 MiB legacy heap을 먼저 요구**

   [`main()`](https://github.com/safl/upcie/blob/cbcccc94ba426961982e0e30d9734c95794e5481/example/upcie_nvme_driver.c#L332-L355)은
   backend를 선택하기 전에 `rte_init()`을 호출하고, 이 함수는 별도의 128 MiB
   `hostmem_heap`을 만든다. dmamem backend는 자체 8 MiB memfd를 사용하지만
   legacy hugepage 준비가 실패하면 새 backend도 시작하지 못한다. 이 경로에서
   legacy heap은 실제 데이터 전송에 쓰이지 않으므로 backend 선택 뒤 필요한
   allocator만 초기화하는 편이 맞다.

9. **낮은 우선순위: 오류 부호와 teardown 결과 손실**

   dmamem 분기는 이미 음수인 `err`를 다시 `-err`로 반환해 함수의 negative
   errno 관례를 깨뜨린다. 프로세스 종료값은 여전히 nonzero지만 상위 caller가
   errno 형식으로 소비하면 의미가 달라진다. cleanup은 controller close의
   quiesce/detach 오류도 버리므로 종료 회귀가 smoke test의 성공 여부에
   반영되지 않는다.

### #37의 locking과 실패 수명

정상 성공 경로의 상태 flag와 역순 cleanup은 이해하기 쉽다. 다만 다음
불변식은 타입이나 refcount로 강제되지 않는다.

```text
controller와 device attach
        < dmamem_heap
        < dmamem과 IOAS mapping
        < IOAS
        < /dev/iommu fd
```

`dmamem_destroy()`는 IOAS unmap이 실패해도 로그만 남기고 CPU mmap과 fd를
계속 해제하며 반환형도 `void`다. 따라서 잘못된 순서로 destroy했거나 장치가
아직 DMA 중이면 retry에 필요한 상태가 사라진다. 이는 코드에서 확인되는
동작이고, 그 결과 늦은 DMA가 해제된 범위에 도달할 수 있다는 부분은 장치
상태에 의존하는 위험 추론이다.

allocator의 freelist와 qpair의 SQ/CQ index에는 lock이 없다. #37의 driver와
smoke test는 한 thread에서 동기적으로 호출하므로 현재 예제는 이 전제를
지킨다. public API 차원에서는 single-thread ownership을 명시하거나 외부 lock
조건을 적어야 한다. 특히 고정 CID를 쓰는 admin helper는 동시에 두 명령을
제출하는 용도로 사용할 수 없다.

### #37의 bisectability와 테스트 범위

다섯 커밋을 각각 clean archive에서 기본 `meson setup`, `meson compile`,
`git diff-tree --check`로 검사했고 모두 통과했다. 즉 기본 toolchain에서
각 커밋은 독립적인 compile bisect point다.

추가된 검증은 두 층이다.

| 항목 | memfd smoke | NVMe Identify smoke |
|---|---:|---:|
| `/dev/iommu`와 IOAS 생성 | O | O |
| `MAP_FILE` | O | O |
| CPU mmap read/write | O | O |
| VFIO device attach | X | O |
| 실제 장치 DMA | X | O, 4 KiB 한 번 |
| allocator edge case | X | X |
| constructor fault injection | X | X |
| enable/disable timeout | X | X |
| I/O qpair | X | X |

memfd smoke가 비교하는 두 IOVA도 결국 같은 `base_iova + offset` 산술에서
나오므로 독립적인 주소 검증은 아니다. Identify smoke는 실제 DMA와 성공 CQE를
확인하지만 CID 일치, 전체 Identify 내용, teardown 성공은 검사하지 않는다.

또한 새 Cijoe YAML은 수동 hardware task일 뿐 표준
`.github/workflows/verify.yml`에서 호출되지 않는다. 최종 #37에서
`meson test`도 `No tests defined`다. 그러므로 **빌드 가능**과 **feature가
자동 검증됨**을 구분해야 한다.

---

## 4. PR #38: NVMe에서 GPU BAR로 DMA

### 한 줄 요약

GPU의 BAR 범위를 dma-buf로 export하고 NVMe와 같은 IOAS에 넣어, NVMe가 그
영역으로 직접 DMA하는 경로를 시연한다.

```text
GPU를 vfio-pci에 bind
  -> GPU BAR를 dma-buf fd로 export
  -> dma-buf를 NVMe의 IOAS에 map
  -> 반환된 IOVA를 NVMe PRP에 기록
  -> IDENTIFY 또는 READ 결과가 GPU BAR에 도착
```

이 PR의 실제 증분은 #37 head `cbcccc9`에서 #38 head `ad60627`까지의 5개
커밋, 11개 파일, `+797/-0`이다. 핵심 질문은 “VFIO가 export한 device BAR
dma-buf를 `IOMMU_IOAS_MAP_FILE`로 다른 VFIO device와 같은 IOAS에 넣을 수
있는가?”다.

### 커밋별 구성과 의존 관계

| 순서 | 커밋 | 추가 기능 | 의존 관계 |
|---:|---|---|---|
| 1 | [`a6974cf`](https://github.com/safl/upcie/commit/a6974cf) | dma-buf fd를 dmamem으로 만드는 constructor | #37 IOAS와 dmamem |
| 2 | [`418da89`](https://github.com/safl/upcie/commit/418da89) | VFIO BAR 한 범위를 dma-buf로 export | VFIO DMA_BUF UAPI |
| 3 | [`ecf3163`](https://github.com/safl/upcie/commit/ecf3163) | GPU와 NVMe를 한 IOAS에 붙여 Identify를 BAR로 전송 | 앞의 두 기능 |
| 4 | [`9b105d0`](https://github.com/safl/upcie/commit/9b105d0) | dmamem 기반 I/O qpair create/delete | #37 admin qpair |
| 5 | [`ad60627`](https://github.com/safl/upcie/commit/ad60627) | namespace READ를 BAR IOVA로 전송 | I/O qpair와 BAR PoC |

첫 두 커밋은 재사용 가능한 primitive이고, 나머지 세 커밋은 그것을 NVMe/GPU
실험으로 묶는다. generic dma-buf constructor의 API 모양은 넓지만, 이 PR이
끝까지 검증하는 exporter는 VFIO BAR 하나뿐이다.

### `dmamem_from_dmabuf()`: fd ownership이 바뀌는 지점

[`dmamem_dmabuf.h`](https://github.com/safl/upcie/blob/ad60627e0490407d7c9eb3056b5d74a419479852/include/upcie/dmamem_dmabuf.h#L41-L82)는
다음 순서로 기존 dma-buf fd를 IOAS에 넣는다.

```text
인자와 size 확인
  -> descriptor를 DMABUF backing으로 초기화
  -> mmap(PROT_READ|PROT_WRITE, MAP_SHARED) 시도
       실패: cpu_va=NULL로 두고 계속 진행
  -> IOMMU_IOAS_MAP_FILE(fd, offset=0, size)
  -> kernel-picked base_iova 저장
```

CPU mmap은 diagnostic 편의 기능이고 필수가 아니다. exporter가 CPU mapping을
지원하지 않아도 offset 기반 IOVA는 사용할 수 있다. 반대로 NVMe queue처럼
CPU가 SQE/CQE를 읽고 써야 하는 메모리에는 `cpu_va != NULL`이 별도
선행조건이다.

fd ownership 계약은 성공 여부에 따라 달라진다.

| 결과 | dma-buf fd | CPU mmap | IOAS mapping |
|---|---|---|---|
| 성공 | `dmamem`으로 소유권 이전 | 가능하면 `dmamem` 소유 | `dmamem` 소유 |
| 실패 | caller가 계속 소유하고 close | constructor가 해제 | 생성되지 않음 |

실패 시 descriptor를 다시 0으로 지우고 `fd=-1`로 만들기 때문에 #37의 memfd
constructor보다 실패 후 상태가 명확하다. 성공 후 destroy 순서는 IOAS unmap,
CPU munmap, fd close다. iommufd와 IOAS 자체는 여전히 caller 소유다.

### VFIO BAR export helper가 실제로 만드는 요청

[`vfio_device_bar_export_dmabuf()`](https://github.com/safl/upcie/blob/ad60627e0490407d7c9eb3056b5d74a419479852/include/upcie/vfioctl.h#L249-L316)는
가변 길이 `struct vfio_device_feature`를 다음과 같이 구성한다.

```text
flags        = VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_DMA_BUF
region_index = 선택한 PCI BAR
open_flags   = O_RDWR | O_CLOEXEC
nr_ranges    = 1
range        = { offset, length }
```

이 ioctl은 일반적인 ioctl처럼 성공 시 0을 반환하지 않고 새 dma-buf fd를
반환하므로 helper가 반환값을 fd로 사용하는 것은 UAPI 계약에 맞다. 오래된
userspace header에 `VFIO_DEVICE_FEATURE_DMA_BUF`가 없으면 같은 함수 이름의
stub이 `-ENOTSUP`을 반환해 build는 유지한다.

다만 당시 kernel 구현은 스스로
[`temporary private interconnect`](https://github.com/torvalds/linux/blob/2b414a95b8f7307d42173ba9e580d6d3e2bcbfce/drivers/vfio/pci/vfio_pci_dmabuf.c)라고
부르고 `nr_ranges != 1`을 `-EOPNOTSUPP`로 거부한다. 즉 “어떤 exporter의
어떤 dma-buf도 바로 import한다”는 것은 constructor가 표현하는 목표이지,
#38에서 검증된 kernel 지원 범위가 아니다.

### 하나의 IOAS에 GPU BAR와 NVMe가 만나는 과정

[`dmamem_nvme_vram_smoketest.c`](https://github.com/safl/upcie/blob/ad60627e0490407d7c9eb3056b5d74a419479852/example/dmamem_nvme_vram_smoketest.c#L29-L226)의
Identify 단계는 세 종류의 자원을 하나의 IOAS에 배치한다.

```text
/dev/iommu
  └─ IOAS
      ├─ GPU VFIO cdev attach
      │    └─ 선택한 GPU BAR range export -> dma-buf
      │          └─ MAP_FILE -> gpu_bar_dmem.base_iova
      ├─ memfd MAP_FILE -> admin_dmem.base_iova
      │    └─ NVMe Admin SQ/CQ backing
      └─ NVMe VFIO cdev attach
           └─ Identify PRP1 = gpu_bar_dmem.base_iova + 0x2000
```

NVMe는 PRP1의 IOVA로 Identify data를 쓴다. CPU는 별도로 mmap한 GPU BAR의
동일 offset에서 serial/model/firmware 문자열을 읽는다. 성공한다면 적어도
NVMe가 GPU device region에 도달했고, GPU BAR dma-buf mapping과 NVMe device
attachment가 같은 IOAS에서 동작했다는 증거다.

종료는 NVMe controller를 먼저 disable하고 queue memory를 반환한 뒤 GPU BAR
CPU mmap, GPU dmamem mapping/fd, GPU device attach/fd, IOAS, iommufd 순으로
정리한다. 이 순서를 지켜야 BAR mapping이 살아 있는 동안 NVMe가 접근할
가능성을 차단할 수 있다.

### 오해하기 쉬운 점

이 PR의 "GPU VRAM"은 CUDA나 HIP이 할당한 버퍼가 아니다. GPU가 `vfio-pci`에
묶여 있으므로 해당 GPU의 vendor driver와 CUDA/HIP runtime을 동시에 사용할
수 없다. CPU가 BAR를 mmap해서 결과를 확인한다. 따라서 #38은 GPU 연산
workflow가 아니라 **NVMe -> PCIe peer memory 배관을 증명하는 PoC**다.

더 정확히 말하면 test가 받은 BAR index와 range를 “VRAM aperture”라고
간주한다. 코드 자체는 해당 BAR가 실제 VRAM인지, ReBAR가 기대한 크기로
설정됐는지, `0x2000`과 `0x4000 + transfer length`가 range 안인지 검증하지
않는다. 따라서 성공 결과는 선택한 PCI region으로의 peer DMA를 증명하지만,
그 region의 GPU 메모리 의미는 실행 configuration의 전제다.

### dmamem I/O qpair의 생성과 삭제

[`nvme_controller_create_io_qpair_dmamem()`](https://github.com/safl/upcie/blob/ad60627e0490407d7c9eb3056b5d74a419479852/include/upcie/nvme/nvme_controller_dmamem_vfio.h#L397-L541)은
기존 generic qpair와 달리 caller가 전달한 `dmamem_heap`에서 queue를 만든다.

```text
free QID 검색과 bitmap reserve
  -> SQ 64 KiB와 CQ 64 KiB를 heap에서 할당
  -> CPU VA와 IOVA를 qpair에 기록
  -> CREATE_IO_CQ(CID 2, PC=1)
  -> CREATE_IO_SQ(CID 3, PC=1, CQID=qid)
  -> 성공 시 SQ/CQ offset을 caller에게 반환
```

CREATE SQ가 실패하면 CID 4로 DELETE CQ를 시도한 뒤 host qpair를 해제한다.
정상 delete는 DELETE SQ(CID 5), DELETE CQ(CID 6), host allocation 반환, QID
반환 순서다. caller는 delete 때까지 qpair뿐 아니라 SQ/CQ offset, heap,
dmamem, IOAS를 모두 살려 둬야 한다.

#38에서는 아직 qpair request pool과 PRP list scratch가 없다. smoke test가
single-PRP READ를 직접 enqueue하므로 동작하지만 generic multi-page request
builder를 지원하는 완성된 I/O qpair는 아니다. #40 첫 커밋이 이 빈 부분을
추가한다.

### READ smoke가 검증하는 것과 검증하지 않는 것

최종 커밋은 qid 1, depth 32의 queue를 만들고 다음 command를 보낸다.

```text
opcode = NVMe READ
NSID   = 1
SLBA   = 0
NLB    = 7                    # 0-based, 8 logical blocks
PRP1   = GPU BAR IOVA + 0x4000
PRP2   = 0
```

512-byte LBA 장치라면 정확히 4 KiB라 PRP1 하나로 표현할 수 있다. 완료 후
CPU는 BAR mmap의 첫 16바이트를 출력한다. 이것은 실제 I/O command와 peer
write를 시도한다는 점에서 Identify보다 한 단계 진전이다.

하지만 이 test는 controller/namespace identify 결과로 LBA format을 읽지
않고 NSID와 block size를 고정한다. 4 KiB LBA라면 명령은 32 KiB를 요구하지만
PRP2/list가 없고 export range 검증도 없다. 또한 첫 16바이트 중 하나라도
nonzero인지 볼 뿐이고, 전부 0이어도 “inconclusive”로 끝내 성공 exit가
가능하다. 따라서 데이터 무결성 test가 아니라 전송 시도 smoke에 가깝다.

### 먼저 고쳐야 할 부분

1. **High: queue 삭제 실패 뒤에도 메모리와 QID를 재사용**

   DELETE SQ/CQ 명령이 실패하거나 timeout 나도 backing memory와 QID를
   즉시 반환한다. timeout은 명령이 실패했다는 뜻이 아니라 결과를 모른다는
   뜻일 수 있다. 장치가 여전히 queue를 사용하면 새 할당과 DMA가 충돌한다.

   더 구체적으로 CREATE SQ가 실패한 rollback은 DELETE CQ의 결과를 무시하고
   SQ/CQ와 QID를 해제한다. 정상 delete도 두 DELETE 결과 중 첫 오류를
   반환하기는 하지만, 결과와 무관하게 host 자원을 반환한다.

   - **구현 사실**: hardware delete 성공을 확인하지 못해도 free가 실행된다.
   - **리뷰 판단**: timeout 뒤 late completion이나 살아 있는 queue가 있다면
     같은 QID/메모리의 재할당과 충돌할 수 있다.

   안전한 정책은 controller reset이나 확실한 device DMA 차단 전까지 해당
   QID와 allocation을 quarantine하는 것이다.

2. **High: queue depth와 고정 64 KiB SQ 크기가 맞는지 검사하지 않음**

   SQ entry는 64바이트이므로 64 KiB에는 최대 1024개만 들어간다. 더 큰
   depth를 받으면 다음 heap 영역을 덮어쓸 수 있고, depth 0도 안전하지 않다.
   CQ entry 크기, `CAP.MQES`, CPU mapping 존재 여부도 함께 검사해야 한다.

3. **High: admin completion을 제출한 CID와 대조하지 않음**

   `nvme_admin_sync_dmamem()`은 첫 phase-valid CQE를 가져와 status만 확인한다.
   `cpl.cid == cmd.cid`인지 검사하지 않고 queue를 serialize하는 lock도 없다.
   timeout 뒤 늦은 CQE가 남거나 두 caller가 동시에 admin 명령을 쓰면 다음
   명령의 완료로 잘못 귀속될 수 있다. CID 확인만으로 concurrency가 완성되는
   것은 아니지만 최소한 stale completion은 거부해야 한다.

4. **테스트가 데이터 무결성을 확인하지 않음**

   READ 뒤 첫 16바이트가 0이 아닌지만 본다. 우연히 남은 값이나 일부만
   기록된 경우도 통과할 수 있고, LBA 0이 전부 0이면 결과를 판단할 수 없다.
   전용 테스트 namespace나 명시적으로 허용된 LBA에 알려진 패턴을 먼저
   쓰고 전체 버퍼를 비교해야 한다. 파괴적 write를 할 수 없다면 Identify
   결과처럼 기대값을 계산할 수 있는 데이터를 전체 비교해야 한다.

5. **테스트가 NSID 1과 512바이트 LBA를 가정**

   4 KiB sector 장치에서는 같은 명령이 32 KiB 전송이 되지만 PRP는 그만큼
   준비하지 않는다. Identify Namespace 결과로 실제 LBA 크기와 namespace를
   확인해야 한다.

6. **조건부 위험: generic dma-buf CPU mapping의 synchronization 계약**

   `dmamem_from_dmabuf()`가 만드는 `dmem->cpu_va`를 향후 caller가 읽고 쓸 때는
   exporter에 따라 `DMA_BUF_IOCTL_SYNC` begin/end가 필요할 수 있지만 API는
   이를 표현하지 않는다. 다만 현재 VRAM smoke가 결과를 확인하는
   `gpu_bar_va`는 이 CPU mmap이 아니라 VFIO BAR region을 별도로 mmap한
   주소이므로, 이 test의 직접 결함으로 분류해서는 안 된다. generic
   constructor의 향후 CPU consumer에 대한 조건부 계약 문제다.

### #38의 중간 커밋 bisectability

각 commit을 깨끗한 archive에서 default Meson build한 결과는 다음과 같다.

| 커밋 | 기본 build | 결과 해석 |
|---|---:|---|
| `a6974cf` | O | dma-buf constructor 단독 build 가능 |
| `418da89` | O | BAR export helper 단독 build 가능 |
| `ecf3163` | **X** | controller open/close 인자 개수 불일치 |
| `9b105d0` | **X** | 앞 smoke의 compile failure가 계속됨 |
| `ad60627` | O | call site를 고쳐 최종 head 복구 |

[`ecf3163`의 smoke](https://github.com/safl/upcie/blob/ecf31638fb2c36127984eeec1f593d4800f0014c/example/dmamem_nvme_vram_smoketest.c#L163-L164)는
6-argument open에 8개를 전달하고, close도 선언보다 많은 인자를 전달한다.
이 오류가 마지막 커밋에서야 수정되므로 두 중간 commit은 compile bisect
point로 사용할 수 없다. 최종 head build 성공만 보고 series 전체의
bisectability가 좋다고 판단하면 안 된다.

최종 #38에서도 `meson test`는 `No tests defined`다. smoke executable과
Cijoe YAML은 생겼지만 Meson `test()`로 등록되지 않았고 표준
`tasks/test.yaml`에서도 호출하지 않는다. stacked PR의 base가 `main`이
아니어서 workflow의 `pull_request.branches: [main]` 조건에도 걸리지 않는다.

### #38 평가

VFIO BAR export, dma-buf MAP_FILE, 두 device의 shared IOAS를 한 example로
연결했다는 점은 PoC로서 가치가 있다. 반면 queue rollback의 DMA safety와
중간 commit build failure는 merge 전에 반드시 고쳐야 한다. READ test는
“VRAM 데이터 무결성”이라는 강한 결론 대신 “설정된 BAR range로 single-PRP
DMA를 시도해 completion을 받음”으로 결과를 좁혀 표현하는 편이 정확하다.

---

## 5. PR #39: VFIO 공통 코드와 실행 모드 통합

### 한 줄 요약

type1과 iommufd controller에 중복된 PCI bus-master, BAR mmap, reset, enable
코드를 공통 helper로 옮기고 실행할 backend를 환경변수로 고른다.

```text
UPCIE_VFIO_MODE=type1    -> 기존 VFIO group/container 경로
UPCIE_VFIO_MODE=iommufd  -> /dev/iommu + VFIO device cdev 경로
UPCIE_VFIO_MODE=auto     -> 조건을 보고 둘 중 하나 선택
```

공통 helper 추출 자체는 두 backend의 동작을 비슷하게 유지한다. 문제는
`auto`라는 이름이 실제 동작보다 강한 보장을 암시한다는 점이다.

이 PR의 실제 증분은 #38 head `ad60627`에서 #39 head `599d267`까지 2개
커밋, 8개 파일, `+272/-198`이다. 새 DMA 기술을 추가하기보다 #37~#38의
실험 경로를 기존 driver 선택 구조 안에 넣고, VFIO type1과 iommufd가 공유할
수 있는 PCI/NVMe bring-up 부분을 추출한다.

### 두 커밋이 나누는 책임

| 순서 | 커밋 | 변경 |
|---:|---|---|
| 1 | [`72c2a4c`](https://github.com/safl/upcie/commit/72c2a4c) | 별도 dmamem backend를 없애고 `UPCIE_VFIO_MODE`로 VFIO DMA API 선택 |
| 2 | [`599d267`](https://github.com/safl/upcie/commit/599d267) | bus master, BAR0, reset, enable을 공통 VFIO PCI helper로 이동 |

첫 커밋은 **어느 backend를 부를지**를 정리하고, 두 번째 커밋은 선택 뒤
**어떤 controller 준비 코드를 같이 쓸지**를 정리한다. 이 둘을 구분해야
“경로가 통합됐다”는 표현을 과대평가하지 않는다.

### mode 선택은 두 단계다

[`upcie_nvme_driver.c`](https://github.com/safl/upcie/blob/599d2679ef0c9d078af459a3b93f3ff8334dc1b6/example/upcie_nvme_driver.c#L9-L61)는
먼저 장치가 현재 어떤 driver에 bind됐는지 본다.

```text
uio_pci_generic -> 기존 SYSFS/UIO backend
vfio-pci        -> UPCIE_VFIO_MODE를 해석
그 밖의 driver  -> -ENOTSUP
```

`UPCIE_VFIO_MODE`의 의미는 다음과 같다.

| 값 | 선택 | 실패 시 다른 backend 재시도 |
|---|---|---:|
| `type1` | legacy VFIO group/container | X |
| `iommufd` | `/dev/iommu`와 VFIO cdev | X |
| `auto`, unset, 알 수 없는 값 | 단순 probe 결과 | X |

알 수 없는 값은 경고 후 AUTO로 간다. AUTO의 probe는 compile-time에
`IOMMU_IOAS_MAP_FILE` 매크로가 있는지와 runtime에
`access("/dev/iommu", R_OK | W_OK)`가 성공하는지만 확인한다. 실제 NVMe
cdev, bind, attach, IOAS MAP_FILE은 아직 시도하지 않은 시점이다.

### type1과 iommufd의 실제 데이터 흐름 비교

두 경로는 최종적으로 같은 `struct nvme_controller`를 채우지만 그 전의
mapping object와 queue allocator가 다르다.

| 단계 | VFIO type1 | iommufd |
|---|---|---|
| device 발견 | BDF -> IOMMU group | BDF -> `vfio-dev/vfioN` cdev |
| IOMMU object | group + container + TYPE1 | `/dev/iommu` + IOAS |
| memory | host hugepage heap | hugepage memfd dmamem heap |
| DMA map | `VFIO_IOMMU_MAP_DMA` | `IOMMU_IOAS_MAP_FILE` |
| address | hostmem PA/IOVA policy | `base_iova + offset` |
| device 연결 | group에서 device fd 획득 | cdev bind + IOAS attach |
| Admin SQ/CQ | `hostmem_heap` | `dmamem_heap` |
| Identify | generic request pool | 수동 CID와 dmamem buffer |
| 이후 결과 | I/O qpair 생성 | Identify 뒤 즉시 반환 |

type1 흐름을 순서로 펴면 다음과 같다.

```text
BDF -> IOMMU group -> container/group 연결 -> TYPE1 설정
  -> host hugepage를 DMA map
  -> group에서 VFIO device fd
  -> 공통 BAR0/reset helper
  -> hostmem Admin SQ/CQ와 ASQ/ACQ
  -> 공통 enable helper
```

iommufd 흐름은 다음과 같다.

```text
BDF -> VFIO device cdev resolve
  -> /dev/iommu -> IOAS
  -> memfd MAP_FILE -> dmamem heap
  -> cdev bind/IOAS attach
  -> 공통 BAR0/reset helper
  -> dmamem Admin SQ/CQ와 ASQ/ACQ
  -> 공통 enable helper
```

### 공통 helper가 보장하는 세 구간

[`nvme_controller_vfio_pci.h`](https://github.com/safl/upcie/blob/599d2679ef0c9d078af459a3b93f3ff8334dc1b6/include/upcie/nvme/nvme_controller_vfio_pci.h#L27-L167)는
세 함수를 제공한다.

1. `nvme_vfio_pci_acquire_bar0()`는 VFIO config region에서
   `PCI_COMMAND_MASTER`를 켜고 BAR0 정보를 조회해 mmap한 뒤
   `ctrlr->func.bars[0]`을 채운다.
2. `nvme_controller_reset_via_bar0()`는 CAP의 timeout 값을 읽고 `CC.EN=0`을
   기록한 뒤 `CSTS.RDY=0`을 기다린다.
3. caller가 AQA/ASQ/ACQ를 설정한 뒤
   `nvme_controller_enable_via_bar0()`가 CSS, MPS, SQES, CQES와 `CC.EN=1`을
   기록하고 `CSTS.RDY=1`을 기다린다.

중복 제거의 경계가 적절하다. VFIO device fd를 얻는 법, DMA mapping API,
Admin queue allocator는 backend 파일에 남기고, fd를 얻은 뒤 동일해야 하는
PCI config와 MMIO만 공통화한다. 이는 #40에서 type1 controller가 dmamem
qpair helper를 쓰도록 확장할 때도 기반이 된다.

### 수명과 cleanup state

iommufd 쪽 `nvme_dmamem_state`는 다음 순서의 성공 여부를 별도 flag로
추적한다.

```text
iommufd_alive -> ioas_alive -> dmem_alive -> heap_alive
               -> ctrlr_alive -> Identify buffer
```

실패와 정상 종료는 buffer free, controller close, heap term, dmamem destroy,
IOAS destroy, iommufd close의 역순이다. 정상 happy path의 ownership은
분명하다. 그러나 controller close가 quiesce에 실패해도 뒤의 memory unmap을
계속하는 #37의 안전성 문제는 refactor로 해결되지 않는다.

type1은 기존 `vfio_ctx`와 controller close를 그대로 사용한다. 즉 공통 helper는
open의 중간 구간을 합쳤지만 resource owner와 rollback 구현까지 하나로 만든
것은 아니다.

### 먼저 고쳐야 할 부분

1. **Medium: AUTO가 실제 fallback을 하지 않음**

   AUTO는 빌드 헤더에 `IOMMU_IOAS_MAP_FILE`이 있고 `/dev/iommu`에 접근할 수
   있으면 iommufd를 선택한다. 이후 다음 단계에서 실패하면 type1로 다시
   시도하지 않고 프로그램이 끝난다.

   예를 들어 `/dev/iommu`는 있지만 해당 PCI 장치에 VFIO cdev가 없거나,
   실행 중인 커널이 MAP_FILE ioctl을 지원하지 않으면 type1은 가능해도
   AUTO는 실패한다. AUTO라면 iommufd 초기화를 실제로 시도한 뒤 지원하지
   않는 오류에서 type1로 재시도해야 한다.

   구체적으로 cdev resolve, IOAS allocation, memfd MAP_FILE, device bind/attach
   중 `ENOENT`, `ENOTTY`, `EOPNOTSUPP`, 권한 오류가 나도
   [`nvme_init()`](https://github.com/safl/upcie/blob/599d2679ef0c9d078af459a3b93f3ff8334dc1b6/example/upcie_nvme_driver.c#L318-L343)은
   오류를 바로 반환한다. 새 header와 구 runtime kernel 조합, `/dev/iommu`는
   있으나 해당 device cdev가 없는 조합에서 type1이 가능해도 실패한다.

   반대 방향의 문제도 있다. 오래된 header로 build하면 새 runtime kernel에
   모든 기능이 있어도 `#ifdef IOMMU_IOAS_MAP_FILE`이 false라 AUTO는 항상
   type1을 고른다. 따라서 이름은 runtime AUTO지만 결과가 build host의 UAPI
   버전에도 종속된다.

2. **Medium: 기존 기본 동작과 환경변수 계약을 바꿈**

   #38까지 vfio-pci의 기본은 type1이고 `UPCIE_BACKEND=dmamem`을 명시할 때만
   iommufd를 탔다. #39부터 `/dev/iommu`가 접근 가능하면 기본 AUTO가 실험적인
   iommufd를 고른다. 기존 `UPCIE_BACKEND`는 더 이상 읽지 않으므로 이전
   실행 script가 조용히 다른 동작을 할 수 있다.

   migration 기간에는 기존 변수를 인식해 deprecation warning을 내거나,
   기본을 type1으로 유지하고 iommufd를 명시적으로 opt-in시키거나, release
   note에 breaking change를 적어야 한다.

3. **Medium: 선택한 backend에 따라 초기화 결과가 다름**

   type1 경로는 I/O qpair까지 만들지만 iommufd 경로는 Identify 뒤에
   반환한다. 따라서 caller가 성공한 `nvme_init()` 뒤 `nvme->ioq`를 사용할 수
   있는지는 선택된 backend에 따라 달라진다. “VFIO mode만 바뀐다”는 API
   인상과 맞지 않으며, 공통 최소 결과를 정의하거나 기능 flag를 반환해야 한다.

4. **Medium: iommufd에도 쓰지 않는 legacy heap이 선행조건**

   `main()`은 mode를 고르기 전에 항상 128 MiB `hostmem_heap`을 초기화한다.
   iommufd path는 별도 memfd heap만 쓰는데도 hugepage 확보가 실패하면 진입하지
   못한다. #37에서 시작된 통합 문제이며 mode 통합 시점에 제거하기 좋은
   대상이다.

5. **High: enable timeout 뒤의 DMA 수명 문제를 그대로 공유**

   공통 enable helper가 `CC.EN=1`을 쓴 뒤 RDY timeout을 반환하면 iommufd
   caller는 명시적인 disable/reset 없이 Admin queue memory를 반환하고 device를
   detach한다. [실패 경로](https://github.com/safl/upcie/blob/599d2679ef0c9d078af459a3b93f3ff8334dc1b6/include/upcie/nvme/nvme_controller_dmamem_vfio.h#L250-L264)의
   순서를 공통화와 별개로 고쳐야 한다. 공통 helper가 timeout 뒤 controller의
   상태를 함께 반환하거나, caller가 detach로 DMA를 차단한 후 memory를
   반환해야 한다.

### AUTO를 구현할 때 필요한 오류 분류

무조건 모든 실패를 type1로 재시도하는 것도 안전하지 않다. iommufd 경로에서
controller를 이미 enable했거나 장치 state를 바꾼 뒤 실패했다면 먼저 확실히
quiesce/reset해야 한다. 현실적인 AUTO는 다음 단계로 나누는 편이 낫다.

```text
1. side-effect 없는 capability probe
   cdev 존재, UAPI query/probe, 권한, MAP_FILE 지원 확인

2. iommufd object와 mapping 준비
   아직 controller MMIO를 바꾸기 전의 EOPNOTSUPP/ENOTTY/ENOENT만 fallback 허용

3. device attach와 controller bring-up
   이 시점 이후 실패는 cleanup/reset 성공을 확인한 뒤에만 type1 재시도
```

forced `iommufd`는 오류를 그대로 반환하고, AUTO만 위의 제한된 오류 집합에서
fallback해야 원인 은폐와 이중 초기화를 피할 수 있다.

### #39의 bisectability와 테스트 범위

두 커밋 `72c2a4c`, `599d267`을 각각 clean archive에서 default Meson build하고
`diff-tree --check`를 실행했으며 둘 다 통과했다. 따라서 compile 관점에서는
독립적으로 bisect 가능하다. 최종 head의 `meson test`는 여전히
`No tests defined`다.

task는 forced iommufd와 forced type1 실행만 다룬다. 다음 case는 없다.

- AUTO가 iommufd를 고르는 환경과 type1을 고르는 환경
- header와 runtime kernel의 UAPI version이 다른 조합
- `/dev/iommu`는 열리지만 cdev/MAP_FILE/attach가 실패하는 조합
- iommufd 실패 뒤 type1 fallback 기대 동작
- 두 mode가 같은 postcondition과 I/O qpair를 제공하는지 비교

stacked PR라 GitHub workflow의 main-base filter에도 걸리지 않는다. 공통 helper
refactor의 compile 성공은 확인됐지만, type1과 iommufd가 같은 controller
register 순서와 failure cleanup을 보인다는 회귀 test는 없다.

### #39 평가

VFIO fd를 얻은 뒤 bus master/BAR/reset/enable이 같아야 한다는 경계를 찾은
refactor는 타당하다. 그러나 mode 통합은 아직 UI 수준의 dispatch에 가깝고,
memory model, request API, 성공 postcondition은 다르다. AUTO fallback과 기존
환경변수 호환성, 두 backend의 결과 대칭성을 정리해야 “교체 가능한 mode”라는
계약이 성립한다.

---

## 6. PR #40: 하나의 dmamem으로 host, CUDA, HIP까지 표현

### 한 줄 요약

`dmamem`에 ARITHMETIC/LUT 변환 방식을 넣고 host hugepage, CUDA, HIP 메모리를
감싸는 constructor와 UIO/type1 controller를 추가한다.

이 PR부터 `dmamem`은 단순히 "iommufd로 매핑한 연속 IOVA"만 뜻하지 않는다.
IOMMU가 없을 때 페이지별 PA를 찾는 LUT도 같은 API 뒤에 숨긴다.

```text
ARITHMETIC
  memfd/dma-buf/hostmem을 iommufd 또는 type1에 map
  -> base_iova + offset

LUT
  host/CUDA/HIP heap이 이미 가진 phys_lut를 빌림
  -> phys_lut[page] + page_offset
```

CUDA/HIP constructor는 메모리를 새로 만들거나 IOMMU에 매핑하는 함수가
아니다. 기존 heap과 그 heap이 가진 물리 주소 표를 빌리는 얇은 wrapper다.

이 PR의 실제 증분은 #39 head `599d267`에서 #40 head `a4f9a4f`까지 8개
커밋, 22개 파일, `+1379/-93`이다. 단일 기능이라기보다 주소 translator,
기존 memory wrapper, request pool, 두 controller backend를 한 번에 묶은 큰
series다.

### 8개 커밋과 기능적 의존 관계

| 순서 | 커밋 | 핵심 변경 | 주의할 의존/회귀 |
|---:|---|---|---|
| 1 | [`9026d9d`](https://github.com/safl/upcie/commit/9026d9d) | dmamem qpair request pool과 per-request PRP scratch | 기존 2 MiB heap보다 scratch만 4 MiB |
| 2 | [`284f65b`](https://github.com/safl/upcie/commit/284f65b) | ARITHMETIC/LUT, `owned`, type1 destroy, dmamem PRP builder | translator invariant가 아직 느슨함 |
| 3 | [`319854b`](https://github.com/safl/upcie/commit/319854b) | `phys_lut`를 heap에서 hugepage로 이동 | 기존 zero-PFN 결함을 LUT에 노출 |
| 4 | [`be72922`](https://github.com/safl/upcie/commit/be72922) | hostmem iommufd/type1/LUT wrapper | VRAM heap을 16 MiB로 늘려 앞 회귀 복구 |
| 5 | [`aa65bd2`](https://github.com/safl/upcie/commit/aa65bd2) | hostmem LUT smoke와 Cijoe task | expected가 같은 LUT에 의존 |
| 6 | [`b78b953`](https://github.com/safl/upcie/commit/b78b953) | CUDA/HIP LUT wrapper | CUDA 기본 64 KiB를 거부 |
| 7 | [`13010ae`](https://github.com/safl/upcie/commit/13010ae) | UIO와 type1 dmamem controller | backend/domain 검증이 없음 |
| 8 | [`a4f9a4f`](https://github.com/safl/upcie/commit/a4f9a4f) | version 0.5.2 | 기능 변경 없음 |

첫 커밋은 #37부터 비어 있던 request pool을 채운다. 두 번째부터 여섯 번째가
memory abstraction을 일반화하고, 일곱 번째가 이를 실제 controller에
연결한다. 따라서 reviewer가 마지막 controller 파일만 보면 translator와
request scratch의 선행 오류를 놓치기 쉽다.

### 최종 `struct dmamem`이 표현하는 상태

[`dmamem.h`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/dmamem.h#L60-L107)의
필드는 네 그룹으로 읽으면 쉽다.

| 그룹 | 필드 | 의미 |
|---|---|---|
| backing | `fd`, `cpu_va`, `size`, `backing`, `owned` | 실제 메모리와 소유권 |
| iommufd map | `iommufd`, `ioas_id`, `base_iova` | IOAS mapping과 연속 IOVA |
| type1 map | `vfio_container`, `base_iova` | container mapping과 연속 IOVA |
| LUT | `phys_lut`, `hugepgsz`, `hugepgsz_shift` | page별 PA와 index 규칙 |

의도상 iommufd, VFIO container, LUT 중 하나만 활성화된다.

```text
iommufd != NULL       -> destroy에서 IOAS_UNMAP
vfio_container != NULL -> destroy에서 VFIO_IOMMU_UNMAP_DMA
둘 다 NULL + LUT      -> kernel mapping이 없으므로 unmap 없음
```

`owned`는 mapping을 제거할지 결정하는 flag가 아니다. memfd/dma-buf의 fd와
CPU mmap을 destroy가 닫을지를 결정한다. host wrapper는 `owned=0`이어도 자신이
설치한 iommufd/type1 mapping은 제거하고, 원래 host allocation만 남겨 둔다.

### translator의 fast path와 필요한 invariant

[`dmamem_offset_to_iova()`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/dmamem.h#L168-L185)는
submission hot path를 매우 짧게 만든다.

```c
if (translator == LUT)
    return phys_lut[offset >> shift] + (offset & (pagesize - 1));
return base_iova + offset;
```

fast path 자체가 unchecked인 설계는 가능하다. 그러려면 constructor가 최소한
다음을 한 번 검증해 descriptor invariant로 고정해야 한다.

- `offset`을 받을 전체 `size`와 실제 LUT entry 수의 관계
- page size가 2의 거듭제곱이고 shift/mask와 일치하는지
- 각 LUT entry와 intra-page addition이 `uint64_t`에서 overflow하지 않는지
- ARITHMETIC의 `base_iova + size - 1`이 overflow하지 않는지
- mapping owner와 실제 controller domain이 같은지

현재 `dmamem`에는 LUT 길이 필드조차 없고 fast path도 offset bounds를 보지
않는다. 따라서 invalid descriptor나 out-of-range caller를 발견할 지점이 없다.

### host memory를 감싸는 세 constructor

[`dmamem_hostmem.h`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/dmamem_hostmem.h#L45-L159)는
같은 `hostmem_hugepage`를 세 방식으로 노출한다.

#### host + iommufd

```text
hostmem_hugepage_alloc -> hp->virt/hp->size
  -> IOMMU_IOAS_MAP(user_va=hp->virt, length=hp->size)
  -> kernel-picked base_iova
  -> owned=0, ARITHMETIC
```

wrapper가 host allocation을 소유하지는 않지만 IOAS mapping은 설치하므로
destroy에서 제거한다. source hugepage와 iommufd/IOAS가 wrapper보다 오래
살아야 한다.

#### host + VFIO type1

```text
caller가 container/group/TYPE1 준비
  -> VFIO_IOMMU_MAP_DMA(vaddr=hp->virt,
                        iova=caller-selected base_iova)
  -> owned=0, ARITHMETIC
```

type1에는 kernel이 빈 IOVA를 골라 주는 mode가 없어 base를 caller가 정한다.
주소 충돌과 aperture 검증도 caller 책임이다. destroy는 borrowed container에
UNMAP_DMA를 보내고 source allocation은 그대로 둔다.

#### host + UIO/no-IOMMU

```text
hugepage allocation 때 /proc/self/pagemap으로 page별 PA 수집
  -> dmamem이 hp->phys_lut를 borrow
  -> IOMMU ioctl 없이 owned=0, LUT
  -> offset마다 PA를 계산해 NVMe PRP로 사용
```

이 mode는 device가 PA를 직접 받는 `iommu=pt/off` 환경을 전제로 한다. LUT
wrapper는 destroy 때 할 kernel mapping이 없고 descriptor만 지운다.

### CUDA/HIP wrapper가 의도하는 데이터 흐름

[`dmamem_cuda.h`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/dmamem_cuda.h#L39-L66)와
HIP sibling은 기존 GPU heap이 가진 device-page PA LUT를 빌린다.

```text
CUDA/HIP virtual allocation
  -> 기존 GPU heap이 page별 device PA를 수집
  -> dmamem_from_{cuda,hip}_lut가 LUT와 page size를 borrow
  -> caller가 GPU VA 안의 offset을 계산
  -> dmamem_offset_to_iova(offset)로 NVMe PRP 생성
```

wrapper의 `cpu_va`는 의도적으로 NULL이다. GPU runtime pointer를 일반 CPU
pointer처럼 dereference하거나 queue backing으로 쓰지 말고, data buffer의
offset만 translator에 넣으라는 뜻이다. source CUDA/HIP heap, 그 LUT와 config는
wrapper와 모든 진행 중 DMA보다 오래 살아야 하지만 refcount는 없다.

이 조합을 GPU allocation부터 UIO NVMe READ/WRITE까지 끝까지 잇는 example은
없다. #38은 VFIO BAR dma-buf+iommufd이고, #40 CUDA/HIP wrapper는 LUT+UIO를
의도하므로 서로 다른 경로다.

### request pool과 PRP scratch가 추가되는 방식

#37~#39의 dmamem qpair에는 request pool이 없어 generic submit helper를 쓸 수
없었다. `9026d9d`는 qpair마다 host-side `nvme_request_pool`을 `calloc()`하고,
같은 dmamem heap에서 request 1024개 각각의 4 KiB PRP list page를 마련한다.

```text
SQ  = 64 KiB
CQ  = 64 KiB
PRP = 1024 * 4 KiB = 4 MiB
rpool metadata = host calloc
```

각 request는 `reqs[i].prp`와 `reqs[i].prp_addr`를 하나씩 받는다. 이로써
generic contig와 iovec submit path를 재사용하려는 방향은 맞지만, LUT에서는
“다음 4 KiB의 PA가 현재 PA+4096”이 아니므로 초기화 방법이 translator 모델과
충돌한다.

### 세 dmamem controller의 공통점과 차이

최종 #40에는 다음 세 controller가 같은 qpair/admin helper를 쓴다.

| controller | device/BAR 획득 | queue DMA 주소 | 외부 소유 자원 |
|---|---|---|---|
| VFIO cdev+iommufd | cdev bind/attach, VFIO BAR mmap | IOAS IOVA | iommufd, IOAS, heap |
| VFIO group+type1 | group device fd, VFIO BAR mmap | type1 IOVA | container, group, heap |
| UIO | sysfs `resource0` mmap | LUT의 PA | source heap/LUT, dmamem heap |

VFIO 두 경로는 #39의 acquire helper를 거쳐 PCI bus master를 켠다. UIO는
`pci_func_open()`과 `pci_bar_map()`으로 BAR0만 연 뒤 동일 reset/AQ/enable
helper로 합류한다. open 이후 CREATE/DELETE qpair 코드는 mode를 보지 않는다.

각 controller context는 BAR/device handle과 Admin SQ/CQ/PRP offset을 소유하고,
mapping context와 heap은 빌린다. 올바른 수명은 다음과 같다.

```text
I/O requests 완료
  -> I/O qpair delete
  -> controller disable과 Admin queue 반환
  -> dmamem heap metadata 제거
  -> dmamem mapping 제거
  -> source allocation/container/IOAS 제거
```

### 먼저 고쳐야 할 부분

1. **High: UIO 경로가 PCI Bus Master Enable을 보장하지 않음**

   UIO controller는 BAR만 mmap하고 PCI command의 Bus Master bit를 켜지
   않는다. `uio_pci_generic`도 이를 대신 보장하지 않는다. 외부에서 BME를
   이미 켜두지 않았다면 NVMe가 SQ/CQ에 DMA하지 못해 Admin 명령이 timeout
   날 수 있다.

   [`nvme_controller_open_dmamem_uio()`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/nvme/nvme_controller_dmamem_uio.h#L80-L112)는
   `pci_func_open()`과 BAR mmap만 한다. VFIO sibling은 공통 helper에서
   `PCI_COMMAND_MASTER`를 명시적으로 설정한다. upstream
   [`uio_pci_generic`](https://github.com/torvalds/linux/blob/2b414a95b8f7307d42173ba9e580d6d3e2bcbfce/drivers/uio/uio_pci_generic.c)도
   probe에서 `pcim_enable_device()`만 사용하며 `pci_set_master()`를 호출하지
   않고 release에서는 master를 clear한다.

   - **구현 사실**: 이 경로 어디에도 BME를 켜는 코드가 없다.
   - **직접 결과**: 외부 설정으로 BME가 이미 1이 아닌 장치에서는 NVMe DMA를
     시작할 수 없다.

   UIO open이 config space를 통해 BME를 설정하고 close 때 원상복구하거나,
   최소한 read-back으로 선행조건을 확인해 명확히 실패해야 한다.

2. **High: 기본 CUDA page size를 importer가 거부**

   LUT helper는 4 KiB, 2 MiB, 1 GiB만 받지만 `cudamem_config_init()`의 기본
   device page size는 64 KiB다. 기본 설정으로 만든 CUDA heap을 import하면
   `-EINVAL`이 된다.

   [`dmamem_lut_pagesize_shift()`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/dmamem.h#L147-L166)는
   세 값만 허용하고,
   [`cudamem_config_init()`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/cudamem_config.h#L112-L120)은
   기본값을 65536으로 설정한다. CUDA constructor가 그 값을 그대로 helper에
   넣으므로 사용자 입력 없이 정상 생성한 heap도 항상 `-EINVAL`이다.

   shift 계산은 generic power-of-two check와 `ctz`로 64 KiB를 포함시키거나,
   CUDA allocator가 실제 PA LUT granularity를 별도로 노출해야 한다.

3. **High: 권한 없는 pagemap 결과를 PA 0으로 오인**

   현대 Linux는 권한이 없을 때 pagemap read 자체를 실패시키는 대신 PFN을
   0으로 가릴 수 있다. 현재 코드는 present bit만 확인하므로 0으로 채워진
   LUT도 성공으로 처리한다. UIO 경로에서 NVMe에 PA 0 근처를 줄 수 있다.
   자세한 동작은 [Linux pagemap 문서](https://docs.kernel.org/admin-guide/mm/pagemap.html)를
   참고한다.

   [`hostmem_pagemap_virt_to_phys()`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/hostmem.h#L38-L72)는
   present bit만 확인하고 PFN 0을 유효한 값으로 반환한다. allocation 쪽은
   오직 `-EPERM`만 “LUT를 만들지 못함”으로 취급한다. Linux 4.2 이후
   `CAP_SYS_ADMIN`이 없는 process는 open/pread 실패가 아니라 PFN bit가 0으로
   가려질 수 있으므로 이 분기는 권한 부재를 검출하지 못한다.

   - **확정된 코드 결과**: non-NULL이지만 모든 entry가 0인 LUT를 만들 수 있다.
   - **위험 결과**: UIO translator는 이를 PA 0 + page offset으로 NVMe에 준다.

   smoke test도 expected PA를 같은 `hp->phys_lut`에서 계산하므로 zero LUT를
   독립적으로 발견하지 못한다. capability를 먼저 검사하고 PFN 0의 의미를
   명확히 처리하며, 실제 device DMA나 독립 source로 결과를 검증해야 한다.

4. **High: PRP scratch 주소가 LUT의 불연속성을 무시**

   첫 request의 IOVA만 번역한 뒤 나머지를 `first + i * 4096`으로 만든다.
   hugepage 경계에서 다음 물리 페이지가 연속이라는 보장이 없으므로 각
   request offset을 `dmamem_offset_to_iova()`로 따로 변환해야 한다.

   [`nvme_request_pool_init_prps_dmamem()`](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/nvme/nvme_request.h#L120-L148)은
   scratch 시작 offset을 한 번만 변환한 뒤 1024개 request에
   `prps_iova + i * 4096`을 기록한다. 총 4 MiB이므로 기본 2 MiB host hugepage
   두 개 이상을 반드시 가로지른다. 다음 hugepage의 PA가 불연속이면 두 번째
   경계부터 틀린 주소다.

   각 `prp_offset + i * 4096`을 translator로 개별 변환해야 한다. 또는 scratch
   자체를 연속 IOVA mapping에만 두고 LUT data heap과 queue/request heap을
   분리해야 한다.

5. **High: iovec용 PRP list에 크기 제한이 없음**

   `dvec_cnt == 0`이어도 첫 원소를 읽고, PRP entry가 한 페이지의 512개를
   넘는지 확인하지 않는다. 큰 정상 입력만으로 request scratch 다음 메모리를
   덮어쓸 수 있다. 함수가 오류를 반환할 수 있도록 API도 바꿔야 한다.

   [문제 함수](https://github.com/safl/upcie/blob/a4f9a4ff84f34c810ed2de6f793124cabf14019b/include/upcie/nvme/nvme_request.h#L486-L521)는
   `dvec_cnt`를 보기 전에 `dvec[0]`을 읽는다. 이후 `prp_idx`에 상한이 없어
   4 KiB PRP page의 512개 `uint64_t` entry를 넘겨 host memory를 쓴다. 함수가
   `void`라 caller에게 `-E2BIG`나 `-EINVAL`을 전달할 수도 없다.

   contiguous builder도 page 수 제한이 주로 `assert`에 의존하고 0-byte와
   `nbytes` arithmetic overflow를 방어하지 않는다. release build에서 assert가
   사라져도 안전한 checked builder가 필요하다.

6. **High: controller 또는 queue 종료 실패 뒤에도 DMA 메모리 해제**

   RDY=0 대기나 DELETE SQ/CQ가 실패해도 SQ/CQ/PRP 메모리를 heap에 돌려준다.
   장치가 아직 살아 있으면 이후 재할당된 메모리로 DMA할 수 있다. 이 문제는
   #38에서 시작되어 #40의 UIO/type1 경로에도 반복된다.

   open에서 `CC.EN=1` 뒤 RDY timeout이 나도 fail label이 Admin SQ/CQ/PRP를
   free한다. close에서도 RDY=0 wait 실패를 반환값에 기록할 뿐 동일하게
   free/unmap을 계속한다. CREATE SQ rollback과 DELETE SQ/CQ 실패도 마찬가지다.

   **구현 사실**은 quiesce 성공 여부와 allocation 반환이 연결돼 있지 않다는
   것이다. **리뷰 판단**은 장치가 실제로 살아 있는 상태라면 freed/reused
   memory에 DMA할 수 있다는 것이다. controller reset, VFIO detach, PCI BME
   clear처럼 DMA가 차단됐음을 확인하기 전에는 allocation과 QID를 quarantine해야
   한다.

7. **High: controller와 dmamem mapping domain의 일치를 검사하지 않음**

   UIO API 주석은 LUT heap을 요구하지만 `heap->dmem->translator == LUT`를
   확인하지 않는다. type1 open도 dmem이 같은 `container`에 map됐는지 보지
   않고, iommufd controller도 같은 iommufd/IOAS인지 확인하지 않는다.

   잘못된 조합은 compile되고 Admin queue 주소를 바로 AQA/ASQ/ACQ에 기록한다.
   결과는 device 관점에서 unmapped IOVA 또는 의미 없는 PA다. controller open
   때 `{translator, mapping owner, domain id}` tuple을 검증하고 mismatch를
   `-EXDEV`나 `-EINVAL`로 거부해야 한다.

8. **High: CPU-mappable하지 않은 backing도 queue heap으로 받음**

   `dmamem_heap` 자체는 offset metadata만 관리하므로 CUDA/HIP나 device BAR처럼
   `cpu_va == NULL`인 backing에서도 초기화할 수 있다. 그러나 qpair init은
   SQ/CQ VA를 `memset()`하고 request pool은 PRP page VA에 pointer arithmetic을
   한다. NULL 여부를 확인하지 않으므로 “dmamem이 지원하는 backing”과
   “NVMe queue backing으로 쓸 수 있는 backing”의 범위가 다르다.

   controller API가 `cpu_va != NULL`을 요구하거나, CPU-mappable queue heap과
   GPU/peer data heap을 별도 parameter로 나눠야 한다. CUDA/HIP dmamem은 data
   PRP용으로는 의미가 있지만 Admin SQ/CQ/PRP-list 저장소로는 사용할 수 없다.

9. **Medium: CUDA/HIP wrapper가 generic PRP builder와 바로 연결되지 않음**

   CUDA/HIP wrapper는 `cpu_va=NULL`이고 offset 기반 사용을 전제로 한다. 그러나
   새 contiguous/iovec PRP builder는 data pointer를 받아
   `dmamem_va_to_iova()`를 호출하며, 이 함수는 `cpu_va`가 NULL이면 assert한다.
   즉 caller가 offset을 직접 IOVA로 바꿔 command를 수동 구성할 수는 있지만,
   #40이 추가한 generic request submission path에는 GPU buffer를 그대로 넣을
   수 없다.

   offset+length를 입력으로 받는 PRP builder를 추가하거나 GPU VA와 allocation
   base의 관계를 안전하게 표현해야 한다. 현재 CUDA/HIP constructor는 address
   descriptor까지만 제공하고 end-to-end NVMe I/O API 통합은 완성하지 않는다.

10. **High: translator의 bounds, 길이, overflow 검증이 없음**

   `dmamem_offset_to_iova()`는 `offset < size`를 검사하지 않고 LUT index와
   `base_iova + offset` overflow도 확인하지 않는다. descriptor에는 `nphys`가
   없어 constructor조차 `size <= LUT entries * page_size`를 확인할 수 없다.
   CUDA/HIP wrapper도 heap size와 LUT entry 수의 일관성을 검증하지 않는다.

   hot path 성능 때문에 unchecked 함수를 유지한다면, `lut_entries`를
   descriptor에 넣고 constructor와 public checked API에서 모든 invariant를
   검증한 뒤 내부 submission만 unchecked variant를 쓰는 구조가 적절하다.

11. **리뷰 판단: `PC=1` queue가 LUT 경계를 넘을 수 있음**

    CREATE CQ/SQ command는 physically contiguous bit를 1로 설정하고 queue의
    첫 DMA 주소 하나만 전달한다. allocator는 4 KiB alignment만 보장한다.
    현재 초기 allocation 순서에서는 64 KiB SQ/CQ가 우연히 한 hugepage 안에
    들어가지만, fragmentation 후 2 MiB LUT page 경계를 넘으면 다음 PA가
    연속이라는 보장이 없다.

    이는 특정 allocation 배치에 의존하는 **자체 추론**이다. 해결책은 queue가
    한 물리 연속 extent 안에 들어가도록 allocator constraint를 추가하거나,
    IOMMU 연속 IOVA queue heap만 허용하거나, controller가 지원한다면
    non-contiguous queue 모델을 구현하는 것이다.

12. **Medium: borrowed object 수명과 unmap 실패를 복구할 수 없음**

    wrapping dmamem은 source hugepage/CUDA/HIP heap, `phys_lut`, iommufd 또는
    container를 raw pointer로 빌리며 refcount가 없다. source를 먼저 free하면
    dangling pointer다. `dmamem_destroy()`는 unmap 실패를 log만 남기고
    descriptor를 0으로 지우므로 retry할 mapping 정보도 잃는다. owned backing은
    unmap 실패 뒤에도 munmap/close까지 진행한다.

    API는 source가 wrapper보다 오래 살아야 한다는 계약을 명시하고, mapping
    teardown은 오류를 caller에게 반환해야 한다. 장치가 attach된 상태에서
    destroy하지 못하도록 controller/mapping state를 직접 연결하는 방법도
    고려할 수 있다.

13. **낮은 우선순위: import와 진단 코드의 일관성**

    `hostmem_hugepage_import()` 경로는 새 `nphys/phys_lut`을 채우지 않아 alloc로
    만든 hugepage와 달리 LUT wrapper로 감쌀 수 없다. 또한 hostmem constructor의
    debug format 중 `int` 값을 `%zu`로 출력하는 곳이 있어 warning-enabled
    build에서 새 format warning이 난다. 첫 커밋 이후에도 “admin queue has no
    request pool”이라는 예전 주석이 남아 실제 상태와 어긋난다. blocker는
    아니지만 public model과 diagnostics를 혼란스럽게 한다.

### #40의 locking과 concurrency

`dmamem_heap` freelist, qpair SQ/CQ indices, request pool에는 내부 lock이 없다.
고정 CID 2~6을 쓰는 admin helper는 completion CID도 대조하지 않는다. 따라서
현재 구현은 controller당 한 thread가 admin create/delete를 serialize하고,
qpair와 heap도 외부에서 독점한다는 전제가 필요하다.

destroy와 queue delete 사이에도 state handoff나 refcount가 없다. 한 thread가
dmamem/source heap을 해제하는 동안 다른 thread가 offset translation 또는
submission을 하면 use-after-free가 가능하다. multi-thread를 지원하지 않는다면
API 문서에 명시하고 assertion 가능한 state를 두는 것이 최소 요건이다.

### #40의 compile bisectability와 기능 bisectability

8개 커밋을 각각 clean archive에서 default Meson setup/compile하고
`diff-tree --check`를 실행했으며 모두 build에 성공했다. 하지만 실행 가능한
중간 상태라는 의미의 bisectability는 다르다.

`9026d9d`는 qpair마다 4 MiB PRP scratch를 요구한다. 당시 VRAM smoke의 Admin
heap은 2 MiB이므로 allocation이 항상 실패한다. `284f65b`, `319854b`에서도
같고, 네 번째 `be72922`가 heap을 16 MiB로 키워서야 기존 smoke가 다시 실행될
수 있다.

```text
9026d9d  build O, 기존 VRAM smoke runtime X
284f65b  build O, 기존 VRAM smoke runtime X
319854b  build O, 기존 VRAM smoke runtime X
be72922  build O, heap 확대 후 runtime 경로 복구
```

따라서 8개 commit은 compile bisect point이지만 앞의 세 개는 feature 동작을
유지하는 bisect point가 아니다. PRP pool과 필요한 heap size 변경을 같은
commit으로 묶었어야 한다.

### #40 테스트가 실제로 덮는 범위

최종 head도 `meson test`에 등록된 test가 없다. 새 LUT smoke는 2 MiB hugepage
4개에 대해 여러 offset을 번역하지만 expected 값을 같은 `hp->phys_lut`에서
다시 계산한다. 따라서 PFN이 모두 0으로 잘못 수집돼도 actual과 expected가
같아 통과한다. YAML 설명은 sudo wrapper를 언급하지만 실제 command에는
sudo가 없다.

| 영역 | 현재 검증 | 빠진 핵심 case |
|---|---|---|
| host LUT | 산술 smoke | 독립 PA 검증, PFN 0, invalid offset |
| page size | 2 MiB 중심 | 4 KiB, 1 GiB, CUDA 64 KiB |
| request PRP | 일반 build | 2 MiB LUT 경계를 넘는 4 MiB scratch |
| iovec | 없음 | empty, 512-entry 초과, overflow |
| controller | 기존 VFIO smoke 일부 | UIO/type1 open/close와 BME |
| domain | 없음 | wrong translator/IOAS/container 거부 |
| teardown | 정상 위주 | RDY/delete timeout, late completion |
| CUDA/HIP | constructor build | 기본 config와 실제 NVMe data path |
| lifetime | 없음 | source-before-wrapper destroy와 concurrent use |

최소 회귀 test는 hardware 없이도 상당 부분 만들 수 있다. checked translator,
LUT boundary, request builder, constructor matrix는 unit test로 만들고, fake BAR
또는 MMIO shim으로 timeout 후 free 순서를 검사할 수 있다. 실제 hardware
suite는 UIO/type1/iommufd 각각에서 BME, queue create/delete, known-pattern
READ/WRITE와 teardown을 검증해야 한다.

### #40 평가

ARITHMETIC/LUT discriminator와 owned/wrapping 구분은 서로 다른 memory source를
하나의 submit API로 모으는 데 유용하다. 하지만 기본 CUDA path가 시작부터
실패하고, LUT PRP scratch가 틀린 PA를 만들며, pagemap 권한 부재가 PA 0으로
바뀌고, iovec가 host memory를 넘겨 쓰는 문제는 모두 merge blocker다.

UIO/type1 controller를 더 늘리기 전에 translator invariant와 queue/data heap
계약, domain matching, 실패 시 quarantine을 먼저 고정해야 한다. 그 위에 각
backend를 작은 commit으로 올려야 reviewer가 주소 모델과 장치 수명을 따로
검증할 수 있다.

---

## 7. 네 PR을 관통하는 리뷰 기준

코드를 볼 때 다음 질문을 반복하면 핵심 오류를 찾기 쉽다.

### 소유권과 종료 순서

- fd, mmap, IOAS mapping, heap allocation은 누가 소유하는가?
- 생성 도중 실패해도 destroy를 호출할 수 있는가?
- 장치가 완전히 멈춘 뒤에 SQ/CQ와 데이터 메모리를 반환하는가?

### 주소 변환

- `offset < size`를 확인하는가?
- 덧셈과 정렬 계산이 overflow하지 않는가?
- LUT 페이지 경계에서도 매번 새 주소를 조회하는가?
- 해당 DMA 주소가 현재 controller와 같은 IOAS/container에 속하는가?

### backend 간 동작 일치

- type1, iommufd, UIO가 같은 수준까지 초기화되는가?
- AUTO가 선택만 하는가, 실패했을 때 실제 fallback도 하는가?
- 모든 경로에서 PCI Bus Master와 controller reset을 보장하는가?

---

## 8. 빌드와 테스트 상태

PR 네 개 모두 깨끗한 임시 checkout에서 기본 Meson build는 성공했다. 그러나
이 결과만으로 동작을 검증했다고 볼 수는 없다.

- `meson test --list` 결과 등록된 테스트가 0개다.
- 새 dmamem smoke task는 기본 Meson/GitHub 검증 경로에서 실행되지 않는다.
- #37은 GitHub Actions가 실행됐지만, feature branch를 base로 하는 #38~#40은
  workflow의 `branches: [main]` 조건 때문에 실행 기록이 없다.
- GPU/NVMe 실제 장치가 필요한 경로는 일반 빌드만으로 검증할 수 없다.

최소한 다음 테스트를 추가하는 것이 좋다.

1. heap의 `SIZE_MAX`, alignment 3, fragmentation/coalescing 단위 테스트
2. constructor 단계별 실패 후 destroy 테스트
3. queue create/delete timeout과 늦은 completion을 강제로 만드는 실패 테스트
4. AUTO 선택 매트릭스: cdev 없음, MAP_FILE 미지원, 권한 실패, type1 성공
5. 전용 테스트 영역에 알려진 패턴을 기록한 뒤 전체 버퍼 비교
6. LUT hugepage 경계를 넘는 PRP scratch 테스트
7. 기본 설정 그대로 CUDA importer를 호출하는 테스트
8. 권한 없는 pagemap 환경에서 PFN 0을 거부하는 테스트

---

## 9. 추천 리뷰 순서

먼저 #40의 최종 `dmamem.h`를 10분 정도 읽어 최종 API를 파악한다. 그 뒤
실제 변경은 스택 순서대로 본다.

1. **#37**: `iommufd.h` -> `dmamem.h` -> `dmamem_memfd.h` ->
   `dmamem_heap.h` -> `nvme_controller_dmamem_vfio.h`
2. **#38**: `dmamem_dmabuf.h` -> BAR export -> I/O qpair create/delete ->
   VRAM smoke test
3. **#39**: `nvme_controller_vfio_pci.h` -> 두 backend caller ->
   `upcie_nvme_driver.c`의 AUTO 선택
4. **#40**: `dmamem.h` -> host/CUDA/HIP constructor -> `nvme_request.h` ->
   UIO/type1/VFIO controller

가장 먼저 남길 리뷰 코멘트는 #37 allocator overflow가 좋다. 입력만으로
재현할 수 있고, 이후 PR 전체가 이 heap을 사용하므로 영향 범위가 가장 넓다.

---

## 10. 결론

네 PR의 구조적 방향은 일관된다. 메모리 종류와 controller backend의 차이를
`dmamem` 뒤로 숨기면 NVMe 제출 코드를 재사용할 수 있다. #38의 BAR PoC와
#39의 공통 helper 추출도 이 방향을 잘 보여준다.

다만 allocator overflow, hardware queue의 수명, AUTO fallback, LUT/PRP 주소
계산처럼 실행 차단이나 메모리 손상으로 이어질 수 있는 문제가 남아 있어
현재 상태를 merge-ready라고 보기는 어렵다. 먼저 주소 계산과 queue 수명
문제를 고치고 작은 단위 테스트로 동작을 고정한 뒤 실제 NVMe/GPU smoke
test를 확장하는 순서가 안전하다.
