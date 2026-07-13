# IOMMUFD upstream patches and Simon's PRs

## 1. 문서의 목적

이 문서는 upstream IOMMUFD 기능과 RFC가 Simon의 upcie PR #37부터
#40까지에 어떤 기반을 제공하고, 어느 부분을 아직 해결하지 못하며, 향후
어떤 코드를 대체하거나 확장할 수 있는지 정리한다.

검토 기준일은 2026-07-13이며 PR의 의존 관계는 다음과 같다.

```text
main
  └─ #37 dmamem + IOMMUFD/memfd 기반
       └─ #38 DMA-BUF + VFIO GPU BAR P2P
            └─ #39 VFIO type1/IOMMUFD 공통화
                 └─ #40 LUT + host/CUDA/HIP/UIO 경로
```

한 문장으로 요약하면, upstream은 메모리를 IOAS에 넣고 장치를 같은
주소 공간에 연결하는 커널 primitive를 만들었고, Simon의 PR들은 이를
`dmamem`이라는 사용자 공간 추상화로 조합해 NVMe DMA 경로를 만든다.
다만 CUDA 메모리를 IOMMU 보호 아래에서 직접 사용하게 해 줄 일반적인
DMA-BUF/PAL 경로가 아직 없으므로, 현재 코드는 VFIO에 바인딩한 GPU BAR
경로와 IOMMU를 우회하는 CUDA/HIP LUT 경로로 갈라져 있다.

## 2. Simon PR 스택의 역할

| PR | 관찰한 head | 핵심 역할 | upstream과의 관계 |
| --- | --- | --- | --- |
| [#37](https://github.com/safl/upcie/pull/37) | `cbcccc94` | `dmamem`, IOMMUFD helper, memfd constructor, NVMe importer 기반 | IOMMUFD core와 `IOMMU_IOAS_MAP_FILE`을 직접 사용 |
| [#38](https://github.com/safl/upcie/pull/38) | `ad60627e` | DMA-BUF constructor와 VFIO GPU BAR를 이용한 NVMe-to-VRAM P2P | v6.19 VFIO DMA-BUF `IOAS_MAP_FILE` 지원을 직접 사용 |
| [#39](https://github.com/safl/upcie/pull/39) | `599d2679` | 기존 VFIO type1과 IOMMUFD 런타임 경로 공통화 | legacy에서 IOMMUFD로의 사용자 공간 전환 계층 |
| [#40](https://github.com/safl/upcie/pull/40) | `a4f9a4ff` | LUT translator, host/CUDA/HIP importer, UIO sibling | 현재 no-IOMMU 우회 구현이며 PAL과 no-IOMMU RFC의 잠재적 수혜자 |

네 PR은 독립적인 대안이 아니라 누적 스택이다. 따라서 #40만 검토해도
#37의 IOMMUFD 객체 수명, #38의 DMA-BUF 의미, #39의 backend 선택 정책을
함께 검토해야 한다.

## 3. 전체 관계 지도

| Upstream 기능 또는 patch set | 대표 author | 상태 | 실제 patch | Simon PR과의 관계 | 분석해야 하는 이유 |
| --- | --- | --- | --- | --- | --- |
| IOMMUFD core, IOAS, VFIO cdev | Jason Gunthorpe, Kevin Tian | v6.2부터 merge | [base infrastructure commit](https://github.com/torvalds/linux/commit/2ff4bed7fee7) | #37, #39의 직접 기반 | IOAS 생성, device attach, IOVA 관리와 객체 수명의 출발점이다. 모든 IOMMUFD 오류 처리와 teardown 검토가 이 ABI에 종속된다. |
| User HWPT, invalidation, page fault | Joao Martins, Yi Liu, Lu Baolu, Nicolin Chen | v6.7~v6.11 merge | [dirty tracking v6](https://lore.kernel.org/all/20231024135109.73787-1-joao.m.martins@oracle.com/), [invalidation](https://lore.kernel.org/all/20230511143844.22693-8-yi.l.liu@intel.com/), [IOPF v7](https://lore.kernel.org/all/20240616061155.169343-1-baolu.lu@linux.intel.com/) | 현재 직접 사용하지 않음 | SVA, demand paging, guest-managed translation으로 확장할 때 중요하지만 현재 `dmamem`은 고정된 DMA mapping만 만든다. 현 PR의 필수 의존성으로 오해하면 안 된다. |
| `IOMMU_IOAS_MAP_FILE` for memfd | Steve Sistare | v6.13 merge | [`MAP_FILE` patch](https://lore.kernel.org/all/1729861919-234514-8-git-send-email-steven.sistare@oracle.com/) | #37의 직접 기반 | `dmamem_from_memfd()`가 file offset 범위를 IOAS의 연속 IOVA에 넣는 핵심 ABI다. Simon 구현이 upstream을 가장 정석적으로 사용하는 부분이다. |
| `IOMMU_IOAS_CHANGE_PROCESS` | Steve Sistare | v6.13 merge | [`CHANGE_PROCESS` patch](https://lore.kernel.org/all/1731527497-16091-4-git-send-email-steven.sistare@oracle.com/) | 현재 사용하지 않음 | live update나 process handoff와 연결할 수 있지만 #39의 backend 선택은 process ownership 이전과 무관하다. |
| vIOMMU, vDEVICE, SMMUv3 nesting | Nicolin Chen | v6.13 merge | [vIOMMU Part 1 v5](https://lore.kernel.org/all/cover.1729897352.git.nicolinc@nvidia.com/) | 현재 직접 관계 없음 | VM 내부의 guest IOMMU와 nested translation을 위한 기능이다. host NVMe-to-memory DMA 경로인 현재 PR 스택과는 별도 축이다. |
| ARM ITS nested, vEVENTQ | Nicolin Chen, Robin Murphy | v6.15 merge | [ITS nested core](https://lore.kernel.org/all/cover.1740014950.git.nicolinc@nvidia.com/), [vEVENTQ patch](https://lore.kernel.org/all/21acf0751dd5c93846935ee06f93b9c65eff5e04.1741719725.git.nicolinc@nvidia.com/) | 현재 직접 관계 없음 | ARM 가상화에서 interrupt/event delivery를 완성하지만 현재 x86/host P2P 실험의 데이터 mapping을 바꾸지 않는다. |
| PASID, `IOMMU_HW_QUEUE_ALLOC` | Kevin Tian, Yi Liu, Nicolin Chen | v6.15 merge | [PASID attach](https://lore.kernel.org/all/20250321171940.7213-12-yi.l.liu@intel.com/), [HW queue v9](https://lore.kernel.org/all/cover.1752126748.git.nicolinc@nvidia.com/) | 향후 확장 가능 | per-process address space나 device queue별 translation으로 발전할 때 연결될 수 있다. 현재 PR은 PASID나 HW queue object를 사용하지 않는다. |
| VFIO DMA-BUF for `IOAS_MAP_FILE` | Jason Gunthorpe | v6.19 merge | [DMA-BUF importer patch](https://lore.kernel.org/all/8-v2-b2c110338e3f+5c2-iommufd_dmabuf_jgg@nvidia.com/) | #38의 직접 기반 | VFIO가 export한 PCI region DMA-BUF를 file descriptor로 받아 동일 IOAS에 매핑한다. #38의 GPU BAR P2P가 mainline ABI만으로 가능한 이유다. |
| Consolidated/generic page table | Jason Gunthorpe, Alejandro Jimenez | v6.19 merge | [generic page table v5](https://lore.kernel.org/all/0-v5-116c4948af3d+68091-iommu_pt_jgg@nvidia.com/) | #37~#40의 간접 기반 | 드라이버별 page-table 구현을 공통화하고 mapping 비용을 줄이는 커널 내부 기반이다. 사용자 ABI를 직접 바꾸지는 않지만 성능과 지원 장치 범위에 영향을 준다. |
| DMA-BUF mapping types and PAL | Jason Gunthorpe | RFC v1 | [mapping types/PAL RFC v1](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/) | #38의 일반화 및 #40 CUDA 경로의 핵심 후보 | exporter가 DMA 주소 형태와 물리 주소 목록을 명시하게 해 VFIO BAR 한 종류에 묶인 경로를 일반 DMA-BUF로 넓힐 가능성이 있다. 그러나 revoke, lifetime, 보안과 NVIDIA exporter가 해결돼야 한다. |
| IOMMUFD no-IOMMU support | Jacob Pan, Jason Gunthorpe | RFC/v10 계열 | [no-IOMMU cdev v10](https://lore.kernel.org/all/cover.1783360051.git.jacob.pan@linux.microsoft.com/) | #40 UIO/LUT 경로의 대체 또는 정리 후보 | `/proc/pagemap`과 자체 physical LUT 대신 IOMMUFD 객체 모델 및 `GET_PA`류 ABI를 사용할 수 있다. API는 통일해도 DMA isolation이 생기는 것은 아니다. |
| `guest_memfd` mapping | Alexey Kardashevskiy | RFC | [`guest_memfd` mapping RFC](https://lore.kernel.org/all/20260225075211.3353194-1-aik@amd.com/) | 현재 직접 관계 없음 | file-backed confidential guest memory constructor를 추가할 때 #37의 constructor 구조와 유사하게 연결될 수 있다. 일반 host/CUDA 메모리 문제를 바로 해결하지는 않는다. |
| AMD vIOMMU/nested | Suravee Suthikulpanit | 진행 중 | [nested v6](https://lore.kernel.org/all/20260115060814.10692-1-suravee.suthikulpanit@amd.com/), [hardware vIOMMU v3](https://lore.kernel.org/all/20260629153535.15775-1-suravee.suthikulpanit@amd.com/) | 현재 직접 관계 없음 | VM assignment를 AMD에서 완성하는 축이다. host의 NVMe P2P와는 독립적이며 향후 가상 머신 지원 시 분석 대상이 된다. |
| TSM/confidential compute | Aneesh Kumar K.V, Nicolin Chen, Shameer Kolothum | 진행 중 | [TSM ioctl v4](https://lore.kernel.org/all/20260427061005.901854-1-aneesh.kumar@kernel.org/) | 장기 확장 후보 | confidential VM에 장치를 assign할 때 memory ownership와 측정/증명이 추가된다. 현재 PR의 raw memory sharing 가정과 충돌할 수 있다. |
| IOMMUFD kernel live update | Samiullah Khawaja, YiFei Zhu | RFC | [live update RFC v2](https://lore.kernel.org/all/20251202230303.1017519-1-skhawaja@google.com/), [live update v2](https://lore.kernel.org/all/20260427175633.1978233-1-skhawaja@google.com/) | #39 이후 운영 기능 후보 | IOMMUFD 상태를 새 process로 넘기는 문제다. type1/IOMMUFD runtime 선택과는 다른 문제이므로 별도 설계가 필요하다. |

## 4. PR별 상세 연결

### 4.1 [PR #37](https://github.com/safl/upcie/pull/37): merge된 IOMMUFD 기능을 `dmamem`으로 포장

#37은 전체 스택의 기반이다. `dmamem`은 메모리의 소유권과 크기뿐 아니라
offset을 장치가 사용할 DMA 주소로 번역하는 역할을 가진다. memfd 경로는
대략 다음 순서로 동작한다.

```text
memfd/file range
  → IOMMUFD context와 IOAS 생성
  → IOMMU_IOAS_MAP_FILE
  → 연속 IOVA 확보
  → NVMe를 같은 IOAS에 attach
  → dmamem offset을 IOVA로 산술 변환
```

이 경로는 v6.13의 [memfd `MAP_FILE` patch](https://lore.kernel.org/all/1729861919-234514-8-git-send-email-steven.sistare@oracle.com/)를
직접 소비한다. page별 물리
주소를 사용자 공간에서 알아내지 않아도 되고, IOMMU가 장치에 노출할
주소를 통제한다는 점에서 #40의 LUT 경로와 본질적으로 다르다.

반면 user HWPT, page fault, PASID, vIOMMU 같은 기능은 사용하지 않는다.
따라서 #37을 IOMMUFD의 모든 최신 기능을 검증하는 PR로 볼 수는 없다.
정확히는 file-backed host memory를 하나의 IOAS에 고정 mapping하고 NVMe를
attach하는 최소 native IOMMUFD 경로다.

[`Consolidated page table` v5](https://lore.kernel.org/all/0-v5-116c4948af3d+68091-iommu_pt_jgg@nvidia.com/)는
#37의 API 표면에는 드러나지 않지만 mapping의
커널 내부 비용과 지원 가능성을 개선하는 간접 기반이다. 성능을 측정할
때에는 사용자 공간 코드 변경과 커널 page-table 개선 효과를 분리해야
한다.

### 4.2 [PR #38](https://github.com/safl/upcie/pull/38): v6.19 VFIO DMA-BUF 기능의 직접적인 소비자

#38은 VFIO에 바인딩한 GPU PCI BAR 또는 ReBAR region을 DMA-BUF로 export한
뒤 `IOMMU_IOAS_MAP_FILE`로 IOAS에 넣는다. NVMe와 GPU가 같은 IOAS를 보게
해 NVMe READ 결과를 그 region으로 보내는 흐름이다.

```text
GPU bound to vfio-pci
  → VFIO region DMA-BUF export
  → IOMMU_IOAS_MAP_FILE
  → GPU BAR에 대응하는 IOVA

NVMe bound to vfio-pci
  → 같은 IOAS에 attach
  → NVMe PRP가 위 IOVA를 가리킴
```

이는 upstream v6.19의
[VFIO DMA-BUF importer patch](https://lore.kernel.org/all/8-v2-b2c110338e3f+5c2-iommufd_dmabuf_jgg@nvidia.com/)와
정확히 맞물린다. 따라서 #38은
mainline ABI로 PCI region P2P mapping을 구성할 수 있다는 중요한 proof of
concept다.

그러나 이 결과를 CUDA allocation에 대한 GPUDirect Storage 경로로 해석하면
안 된다. GPU를 `vfio-pci`에 바인딩했으므로 일반적인 NVIDIA 커널 드라이버와
CUDA runtime을 동시에 사용할 수 없다. mapping 대상도 CUDA가 할당한
버퍼가 아니라 VFIO가 노출한 PCI region이다.

[DMA-BUF mapping types/PAL RFC v1](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)은
#38의 다음 단계와 직접 관련된다. RFC가
성숙하면 importer는 DMA-BUF가 제공하는 주소의 의미를 협상하고 여러
segment를 IOAS에 넣을 수 있다. 그러나 CUDA 경로가 실제로 열리려면
NVIDIA 드라이버가 CUDA allocation을 적절한 mapping type과 lifetime
규칙을 가진 DMA-BUF로 export해야 한다. PAL ABI만 merge된다고 Simon의
코드가 자동으로 CUDA 지원을 얻는 것은 아니다.

### 4.3 [PR #39](https://github.com/safl/upcie/pull/39): 커널 기능 추가가 아니라 migration 계층

#39는 NVMe VFIO 초기화 코드를 공통화하고 다음 backend를 런타임에 고른다.

- `type1`: `VFIO_IOMMU_MAP_DMA`를 사용하는 기존 경로
- `iommufd`: `IOMMU_IOAS_MAP_FILE`과 IOAS attach를 사용하는 새 경로
- `auto`: 사용 가능하다고 판단한 backend를 선택하는 경로

이 PR의 upstream상 의미는 IOMMUFD가 기존 VFIO type1을 대체해 가는 동안
하나의 애플리케이션이 두 ABI를 함께 지원하도록 하는 것이다. 새 IOMMUFD
kernel feature를 구현하거나 사용하지는 않는다.

특히 [`IOMMU_IOAS_CHANGE_PROCESS` patch](https://lore.kernel.org/all/1731527497-16091-4-git-send-email-steven.sistare@oracle.com/)나
[kernel live update v2](https://lore.kernel.org/all/20260427175633.1978233-1-skhawaja@google.com/)와
혼동하면 안 된다. #39는 같은 process 안에서 초기 backend를 선택한다.
이미 생성한
IOAS와 device state를 다른 process로 넘기거나 실행 중인 서비스를
무중단 교체하지 않는다.

또한 `auto`가 capability 검사 후 한 backend를 선택하는 것과, 실제 초기화
실패 시 자원을 정리하고 다른 backend로 재시도하는 것은 다르다. 이 구분은
지원 kernel 범위를 문서화하고 오류 경로를 검토할 때 중요하다.

### 4.4 [PR #40](https://github.com/safl/upcie/pull/40): IOMMU 경로와 physical LUT 경로가 만나는 지점

#40은 `dmamem`의 주소 변환 방식을 두 종류로 확장한다.

```text
ARITHMETIC
  memfd / VFIO BAR DMA-BUF / host memory
  offset + base IOVA로 변환
  IOMMUFD 또는 VFIO type1 mapping 사용

LUT
  host / CUDA / HIP memory
  page별 physical DMA address를 lookup
  UIO 또는 no-IOMMU 성격의 controller 사용
```

이 구조 덕분에 상위 NVMe 코드는 동일한 `dmamem` 인터페이스를 사용할 수
있다. 그러나 두 translator가 제공하는 보안과 주소 안정성은 같지 않다.
ARITHMETIC 경로의 주소는 IOAS가 관리하는 IOVA이고, CUDA/HIP LUT 경로는
IOMMU 보호 없이 장치에 물리 주소를 전달하는 우회 경로다.

따라서 #40의 CUDA/HIP constructor는 #38의 제한을 해결한 IOMMUFD CUDA
경로가 아니다. CUDA allocation을 PAL DMA-BUF로 export해 IOAS에 넣는
대신, page별 주소 목록을 PRP로 변환해 no-IOMMU DMA에 사용한다. domain
일치, pinning, lifetime, page size, address width와 PRP segment 경계가
모두 명시적으로 검증돼야 한다.

[IOMMUFD no-IOMMU cdev v10](https://lore.kernel.org/all/cover.1783360051.git.jacob.pan@linux.microsoft.com/)은
이 코드를 정리할 가능성이 가장 크다. UIO와
`/proc/pagemap` 기반 자체 구현 대신 IOMMUFD의 device/IOAS 객체 모델을
재사용하고 kernel이 허용한 physical address를 질의할 수 있기 때문이다.
그 경우 #39의 backend 통합도 더 단순해질 수 있다. 하지만 IOMMU가 없는
장치가 임의의 system memory를 DMA할 수 있다는 위험은 그대로이므로,
API 통합을 isolation 제공으로 표현해서는 안 된다.

## 5. 이미 사용할 수 있는 것과 아직 기다려야 하는 것

### 5.1 현재 mainline으로 성립하는 경로

다음은 merge된 기능만으로 구성할 수 있다.

1. memfd를 `IOMMU_IOAS_MAP_FILE`로 mapping하고 NVMe를 attach하는 #37 경로
2. VFIO PCI region DMA-BUF를 IOAS에 mapping하는 #38 경로
3. legacy type1과 IOMMUFD를 선택하는 #39 호환 계층
4. host memory를 IOMMUFD 또는 type1에 mapping하는 #40의 ARITHMETIC 경로

즉 Simon의 IOMMUFD 기반 자체는 단순한 미래 제안이 아니다. memfd와 VFIO
BAR에 대해서는 현재 mainline primitive를 실제로 조합한다.

### 5.2 mainline이지만 현재 PR과 직결되지 않는 기능

vIOMMU, vDEVICE, nesting, vEVENTQ, PASID, HW queue, page fault와 user HWPT는
중요한 upstream 진전이지만 현재 host P2P 경로의 dependency는 아니다.
이 기능들은 VM assignment, SVA, demand paging 또는 queue별 address space를
설계할 때 별도 단계로 분석하는 편이 맞다.

### 5.3 아직 upstream 작업이 필요한 경로

Simon의 최종 목표를 “NVIDIA 드라이버와 CUDA를 유지하면서 NVMe를 같은
IOMMU domain에 넣고 CUDA allocation으로 직접 DMA”라고 정의하면 다음
연결고리가 부족하다.

```text
CUDA allocation
  → NVIDIA exporter가 DMA-BUF와 mapping semantics 제공
  → PAL/mapping type 협상
  → IOMMUFD가 exporter의 ranges를 IOAS에 mapping
  → NVMe가 같은 IOAS의 IOVA를 사용
```

여기에는 PAL RFC의 merge뿐 아니라 exporter 구현, revoke와 lifetime,
multi-range mapping, cache coherency와 P2P topology 검증이 필요하다. 현재
#38은 첫 번째 화살표 대신 VFIO BAR를 사용하고, #40은 IOMMUFD mapping
자체를 우회한다.

## 6. 실행 시나리오로 본 차이

| 시나리오 | GPU driver 상태 | NVMe backend | 주소 형식 | 보호 수준 | 의미 |
| --- | --- | --- | --- | --- | --- |
| #38 VFIO BAR | GPU도 `vfio-pci` | IOMMUFD | 연속 IOVA | IOMMU domain | mainline VFIO DMA-BUF P2P PoC, CUDA 사용 불가 |
| #40 CUDA/HIP LUT | vendor driver/runtime 유지 가능 | UIO/no-IOMMU | physical page LUT | IOMMU isolation 없음 | CUDA buffer 실험 가능, 안전한 IOMMUFD CUDA 경로는 아님 |
| 향후 PAL 경로 | vendor driver/runtime 유지 | VFIO cdev + IOMMUFD | exporter가 협상한 ranges를 IOVA로 mapping | IOMMU domain 목표 | Simon 구조가 지향할 수 있는 정식 CUDA/P2P 경로 |
| 향후 no-IOMMU cdev | 환경에 따라 다름 | IOMMUFD no-IOMMU | kernel이 반환한 PA | isolation 없음 | #40 UIO/LUT API를 현대화하지만 보안 속성은 개선하지 않음 |

이 표에서 가장 중요한 구분은 “CUDA를 사용할 수 있는가”와 “IOMMU로
보호되는가”가 서로 다른 축이라는 점이다. #38은 후자를 만족하지만
전자를 만족하지 못하고, #40 LUT 경로는 전자를 노리지만 후자를 포기한다.
PAL과 적절한 GPU exporter가 필요한 이유는 두 속성을 동시에 얻기
위해서다.

## 7. Simon PR 분석에서 upstream patch를 적용하는 방법

### 7.1 #37과 #38은 merge된 ABI 기준으로 먼저 검증한다

이 두 PR은 미래 RFC가 없어도 평가할 수 있다. 다음 항목이 핵심이다.

- IOAS, device, DMA-BUF와 mapping object의 생성/해제 순서
- partial failure 시 unmap, detach, fd close가 정확히 한 번 수행되는지
- DMA-BUF range와 page alignment, IOVA overflow 검증
- NVMe queue와 `dmamem`의 lifetime 관계
- 실제 kernel version 및 VFIO region capability 확인

### 7.2 #40의 LUT는 별도 보안 모델로 취급한다

LUT를 ARITHMETIC translator의 단순한 성능상 대안으로 보면 안 된다. 문서와
API에 최소한 다음 capability를 분리해서 표현해야 한다.

- CPU mapping 가능 여부
- 주소가 IOVA인지 physical address인지
- 연속 IOVA 보장 여부와 LUT page size
- memory pinning 및 exporter ownership
- cache coherency 요구사항
- 연결할 수 있는 IOMMU domain 또는 no-IOMMU controller 종류

상위 계층이 translator 종류만 보고 PRP를 만들기보다 이러한 capability를
검사하도록 해야 PAL 또는 no-IOMMU ABI가 바뀌어도 `dmamem` 추상화를
유지하기 쉽다.

### 7.3 RFC는 현재 코드를 정당화하는 근거가 아니라 migration 후보로 본다

PAL과 no-IOMMU patch set은 아직 ABI와 보안 검토가 진행 중이다. 따라서
현재 PR의 필수 kernel requirement로 선언하기보다 다음과 같이 추적하는
것이 적절하다.

- PAL: #38의 VFIO BAR 전용 DMA-BUF importer를 일반 exporter로 확장
- NVIDIA PAL exporter: #40 CUDA LUT를 IOMMUFD IOVA 경로로 대체
- no-IOMMU cdev: #40 UIO와 physical-address discovery를 공통 객체로 대체
- live update: 장기적으로 #39 뒤에 process handoff 기능 추가

## 8. 우선순위와 결론

분석 우선순위는 다음과 같다.

1. #37의 memfd/IOMMUFD 객체 수명과 mapping correctness
2. #38의 v6.19 VFIO DMA-BUF range, P2P topology와 queue lifetime
3. #39의 type1/IOMMUFD 선택 및 실패 후 fallback
4. #40의 LUT-to-PRP 변환, pinning, address validity와 no-IOMMU 위험
5. PAL RFC와 NVIDIA exporter가 #38/#40의 양분된 경로를 통합할 수 있는지
6. no-IOMMU RFC가 #40의 실험 경로를 더 명확한 ABI로 옮길 수 있는지

결론적으로 Simon의 PR 스택은 upstream IOMMUFD와 별개의 실험이 아니라,
merge된 memfd와 VFIO DMA-BUF 기능을 실제 NVMe 데이터 경로에 적용한
사용자 공간 소비자다. #37과 #38은 현재 upstream ABI의 직접적인 적용이고,
#39는 전환기 호환 계층이며, #40은 아직 upstream이 제공하지 못하는
CUDA/HIP와 no-IOMMU 사용 사례를 LUT로 탐색한다.

향후 핵심은 기능 수를 늘리는 것이 아니라 #38의 “IOMMU 보호”와 #40의
“CUDA runtime 유지”를 하나의 경로에서 결합하는 것이다. 그 연결점이
DMA-BUF mapping types/PAL과 GPU exporter이며, no-IOMMU RFC는 안전한
대체재가 아니라 #40의 우회 경로를 공통 IOMMUFD API로 표현하는 별도의
선택지다.

## 9. 관련 문서

- [IOMMUFD upstream patch roadmap](./IOMMUFD_UPSTREAM_PATCH_ROADMAP.md)
- [DMA-BUF mapping types RFC v1 analysis](./DMA_BUF_MAPPING_TYPES_RFC_V1_ANALYSIS.md)
- [Simon PR #37~#39 analysis](./iommufd_prs_analysis.md)
- [Simon PR #37~#40 deep dive](./iommufd_prs_deep_dive.md)
- [DMA-BUF mapping types/PAL RFC v1](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)
