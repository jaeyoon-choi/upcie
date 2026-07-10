# upcie PR #37~#40 쉽게 읽는 분석

이 네 PR의 목적은 **NVMe가 host memory와 GPU memory를 같은 방식으로
DMA하도록 주소 변환과 메모리 수명을 `dmamem` 하나에 모으는 것**이다.

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
                       ▼
                 dmamem_heap
             SQ/CQ와 데이터 영역 할당
                       │
                       ▼
             NVMe PRP와 queue 주소
                       │
                       ▼
       iommufd / VFIO type1 / UIO controller
```

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

주요 파일은 다음과 같다.

- `iommufd.h`: `/dev/iommu`와 VFIO cdev ioctl wrapper
- `dmamem.h`: DMA 메모리의 공통 정보와 주소 변환
- `dmamem_memfd.h`: hugepage memfd 생성 및 IOAS 매핑
- `dmamem_heap.h`: 큰 dmamem을 offset 단위로 나누는 allocator
- `nvme_controller_dmamem_vfio.h`: NVMe BAR, Admin SQ/CQ, controller 수명 관리

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

3. **Medium: `hugepgsz=0`이면 오류 반환 대신 SIGFPE**

   0인지 확인하기 전에 `size % hugepgsz`를 계산한다. 지원하는 page size인지
   먼저 검사해야 한다.

4. **계약 확인: 생성에 실패한 dmamem의 상태**

   실제 IOAS 매핑 전에 `size`와 iommufd 정보를 기록한다. 생성 도중 실패한
   객체에 `dmamem_destroy()`를 호출하면 매핑하지 않은 IOVA 0을 unmap하려고
   할 수 있다. 현재 API는 실패한 객체를 destroy해도 되는지 명확히 말하지
   않는다. 실패 후 상태를 초기화하거나, `mapped` 상태를 두거나, cleanup
   계약을 문서화해야 한다.

5. **Medium: controller enable 실패 뒤 메모리를 너무 빨리 반환**

   `CC.EN=1` 이후 RDY timeout이 나도 명시적으로 disable하지 않고 Admin
   SQ/CQ를 heap에 돌려준다. 장치가 늦게 DMA하면 재사용된 메모리를 건드릴
   수 있다.

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

### 오해하기 쉬운 점

이 PR의 "GPU VRAM"은 CUDA나 HIP이 할당한 버퍼가 아니다. GPU가 `vfio-pci`에
묶여 있으므로 해당 GPU의 vendor driver와 CUDA/HIP runtime을 동시에 사용할
수 없다. CPU가 BAR를 mmap해서 결과를 확인한다. 따라서 #38은 GPU 연산
workflow가 아니라 **NVMe -> PCIe peer memory 배관을 증명하는 PoC**다.

### 먼저 고쳐야 할 부분

1. **High: queue 삭제 실패 뒤에도 메모리와 QID를 재사용**

   DELETE SQ/CQ 명령이 실패하거나 timeout 나도 backing memory와 QID를
   즉시 반환한다. timeout은 명령이 실패했다는 뜻이 아니라 결과를 모른다는
   뜻일 수 있다. 장치가 여전히 queue를 사용하면 새 할당과 DMA가 충돌한다.

2. **High: queue depth와 고정 64 KiB SQ 크기가 맞는지 검사하지 않음**

   SQ entry는 64바이트이므로 64 KiB에는 최대 1024개만 들어간다. 더 큰
   depth를 받으면 다음 heap 영역을 덮어쓸 수 있고, depth 0도 안전하지 않다.

3. **테스트가 데이터 무결성을 확인하지 않음**

   READ 뒤 첫 16바이트가 0이 아닌지만 본다. 우연히 남은 값이나 일부만
   기록된 경우도 통과할 수 있고, LBA 0이 전부 0이면 결과를 판단할 수 없다.
   전용 테스트 namespace나 명시적으로 허용된 LBA에 알려진 패턴을 먼저
   쓰고 전체 버퍼를 비교해야 한다. 파괴적 write를 할 수 없다면 Identify
   결과처럼 기대값을 계산할 수 있는 데이터를 전체 비교해야 한다.

4. **테스트가 NSID 1과 512바이트 LBA를 가정**

   4 KiB sector 장치에서는 같은 명령이 32 KiB 전송이 되지만 PRP는 그만큼
   준비하지 않는다. Identify Namespace 결과로 실제 LBA 크기와 namespace를
   확인해야 한다.

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

### 먼저 고쳐야 할 부분

1. **High: AUTO가 실제 fallback을 하지 않음**

   AUTO는 빌드 헤더에 `IOMMU_IOAS_MAP_FILE`이 있고 `/dev/iommu`에 접근할 수
   있으면 iommufd를 선택한다. 이후 다음 단계에서 실패하면 type1로 다시
   시도하지 않고 프로그램이 끝난다.

   예를 들어 `/dev/iommu`는 있지만 해당 PCI 장치에 VFIO cdev가 없거나,
   실행 중인 커널이 MAP_FILE ioctl을 지원하지 않으면 type1은 가능해도
   AUTO는 실패한다. AUTO라면 iommufd 초기화를 실제로 시도한 뒤 지원하지
   않는 오류에서 type1로 재시도해야 한다.

2. **확인 필요: 선택한 backend에 따라 초기화 결과가 다름**

   type1 경로는 I/O qpair까지 만들지만 iommufd 경로는 Identify 뒤에
   반환한다. 이 차이가 실제 caller의 계약에 맞는지 확인해야 한다.

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

### 먼저 고쳐야 할 부분

1. **High: UIO 경로가 PCI Bus Master Enable을 보장하지 않음**

   UIO controller는 BAR만 mmap하고 PCI command의 Bus Master bit를 켜지
   않는다. `uio_pci_generic`도 이를 대신 보장하지 않는다. 외부에서 BME를
   이미 켜두지 않았다면 NVMe가 SQ/CQ에 DMA하지 못해 Admin 명령이 timeout
   날 수 있다.

2. **High: 기본 CUDA page size를 importer가 거부**

   LUT helper는 4 KiB, 2 MiB, 1 GiB만 받지만 `cudamem_config_init()`의 기본
   device page size는 64 KiB다. 기본 설정으로 만든 CUDA heap을 import하면
   `-EINVAL`이 된다.

3. **High: 권한 없는 pagemap 결과를 PA 0으로 오인**

   현대 Linux는 권한이 없을 때 pagemap read 자체를 실패시키는 대신 PFN을
   0으로 가릴 수 있다. 현재 코드는 present bit만 확인하므로 0으로 채워진
   LUT도 성공으로 처리한다. UIO 경로에서 NVMe에 PA 0 근처를 줄 수 있다.
   자세한 동작은 [Linux pagemap 문서](https://docs.kernel.org/admin-guide/mm/pagemap.html)를
   참고한다.

4. **High: PRP scratch 주소가 LUT의 불연속성을 무시**

   첫 request의 IOVA만 번역한 뒤 나머지를 `first + i * 4096`으로 만든다.
   hugepage 경계에서 다음 물리 페이지가 연속이라는 보장이 없으므로 각
   request offset을 `dmamem_offset_to_iova()`로 따로 변환해야 한다.

5. **High: iovec용 PRP list에 크기 제한이 없음**

   `dvec_cnt == 0`이어도 첫 원소를 읽고, PRP entry가 한 페이지의 512개를
   넘는지 확인하지 않는다. 큰 정상 입력만으로 request scratch 다음 메모리를
   덮어쓸 수 있다. 함수가 오류를 반환할 수 있도록 API도 바꿔야 한다.

6. **High: controller 또는 queue 종료 실패 뒤에도 DMA 메모리 해제**

   RDY=0 대기나 DELETE SQ/CQ가 실패해도 SQ/CQ/PRP 메모리를 heap에 돌려준다.
   장치가 아직 살아 있으면 이후 재할당된 메모리로 DMA할 수 있다. 이 문제는
   #38에서 시작되어 #40의 UIO/type1 경로에도 반복된다.

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
