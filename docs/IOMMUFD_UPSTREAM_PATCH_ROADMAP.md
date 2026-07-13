# IOMMUFD upstream patch 분석 로드맵

> 기준 발표: LPC 2025, *IOMMUFD Development Status*
>
> 기준일: 2026-07-13

## 1. 목적

이 문서는 LPC 2025 발표자료에서 확인한 IOMMUFD 기능과 이후 검색한
upstream 패치 시리즈를 하나의 목록으로 정리한다. 각 항목이 이미 mainline에
포함된 기능인지, 아직 설계 검토가 필요한 RFC인지, UPCIE의 NVMe-GPU P2P 및
no-IOMMU 경로와 어떤 관계가 있는지를 구분하는 것이 목적이다.

상세 코드 분석을 모두 반복하지 않고 다음 질문에 답하는 상위 로드맵으로
사용한다.

- IOMMUFD에서 어떤 기능이 이미 제공되는가?
- 앞으로 추적해야 할 RFC와 진행 중인 시리즈는 무엇인가?
- UPCIE에 직접 영향을 주는 항목은 무엇인가?
- 어떤 순서로 별도 분석 문서를 작성해야 하는가?

## 2. LPC 2025에서 확인한 기능

발표자료의 기능표는 다음과 같다. 여기서 `v6.x`는 해당 기능이 들어간 커널
버전이고, `vN`은 당시 패치 시리즈 revision이다. `RFC`와 `N/A`는 아직 확정된
mainline 기능이 아님을 뜻한다.

| 기능 | 발표 당시 상태 |
|---|---:|
| [`IOMMU_IOAS_MAP_FILE` for memfd](https://lore.kernel.org/all/1729861919-234514-8-git-send-email-steven.sistare@oracle.com/) | v6.13 |
| [`IOMMU_IOAS_CHANGE_PROCESS`](https://lore.kernel.org/all/1731527497-16091-4-git-send-email-steven.sistare@oracle.com/) | v6.13 |
| [vIOMMU, vDEVICE, SMMUv3 nesting](https://lore.kernel.org/all/cover.1729897352.git.nicolinc@nvidia.com/) | v6.13 |
| [ARM ITS management with nested](https://lore.kernel.org/all/cover.1740014950.git.nicolinc@nvidia.com/) | v6.15 |
| [vEVENTQ](https://lore.kernel.org/all/cover.1737754129.git.nicolinc@nvidia.com/) | v6.15 |
| [PASID support](https://lore.kernel.org/all/20231127063428.127436-1-yi.l.liu@intel.com/) | v6.15 |
| [`IOMMU_HW_QUEUE_ALLOC`](https://lore.kernel.org/all/cover.1752126748.git.nicolinc@nvidia.com/) | v6.15 |
| [VFIO DMA-BUF for `IOAS_MAP_FILE`](https://lore.kernel.org/all/8-v2-b2c110338e3f+5c2-iommufd_dmabuf_jgg@nvidia.com/) | v6.19 |
| [Consolidated page table](https://lore.kernel.org/all/0-v5-116c4948af3d+68091-iommu_pt_jgg@nvidia.com/) | v6.19 |
| [AMD vIOMMU and nested translation](https://lore.kernel.org/all/20251112182506.7165-1-suravee.suthikulpanit@amd.com/) | v5 |
| [Kernel live update](https://lore.kernel.org/all/20251202230303.1017519-1-skhawaja@google.com/) | RFC |
| [No-IOMMU support](https://lore.kernel.org/all/cover.1783360051.git.jacob.pan@linux.microsoft.com/) | RFC |
| [ARM ITS direct routing](https://lore.kernel.org/all/cover.1736550979.git.nicolinc@nvidia.com/) | v2 |
| [Confidential compute](https://lore.kernel.org/all/20260427061005.901854-1-aneesh.kumar@kernel.org/) | N/A |
| [Page-table optimizations and features](https://lore.kernel.org/all/0-v5-116c4948af3d+68091-iommu_pt_jgg@nvidia.com/) | N/A |

이 표는 발표 시점의 스냅샷이다. 특히 AMD와 no-IOMMU 작업은 이후 여러
revision이 추가됐으므로 현재 상태를 발표자료의 `v5` 또는 `RFC`로만 판단하면
안 된다.

## 3. 분석 범위와 우선순위

분석 대상은 진행 중인 RFC에 한정하지 않는다. 이미 merge된 기능은 현재 사용할
수 있는 UAPI와 객체 lifetime의 기준선이고, RFC는 그 기준선 위에서 무엇을
바꾸려는지 보여주는 delta다. 따라서 다음 두 트랙을 함께 분석해야 한다.

### 3.1 Merge된 기반 기능

아래 author는 merge commit의 maintainer가 아니라 해당 기능을 구현한 대표 patch
또는 patch series의 작성자다. 여러 작성자가 기능을 나눠 구현한 경우 함께
표기했다.

| 우선순위 | 기능 | 대표 author | merge 버전 | 분석 이유 |
|---|---|---|---:|---|
| P0 | [IOMMUFD 기본 객체와 VFIO cdev](https://github.com/torvalds/linux/commit/2ff4bed7fee7) | Jason Gunthorpe, Kevin Tian | v6.2 이후 | 이후 모든 기능이 공유하는 IOAS, HWPT, DEVICE와 object ID 모델의 출발점이다. VFIO type1과 달라진 ownership, auto-domain 생성, attach/replace/detach 및 FD close 시 파괴 순서를 이해해야 이후 RFC의 lifetime 문제를 판단할 수 있다. |
| P1 | [Dirty tracking과 user HWPT](https://lore.kernel.org/all/20231024135109.73787-1-joao.m.martins@oracle.com/) | Joao Martins, Yi Liu | v6.7 | dirty tracking은 device DMA write를 migration bitmap으로 회수하는 경로이고, user HWPT는 guest가 관리하는 stage-1을 hardware에 연결하는 기반이다. migration 정확성과 kernel-managed stage-2/user-managed stage-1의 책임 경계를 함께 확인해야 한다. |
| P1 | [User HWPT invalidation](https://lore.kernel.org/all/20230511143844.22693-8-yi.l.liu@intel.com/) | Yi Liu, Nicolin Chen | v6.8 | userspace가 stage-1 PTE를 바꾼 뒤 어떤 단위와 순서로 IOTLB invalidation을 요청하는지 정의한다. invalidation 배열의 부분 성공, driver별 data type, completion 및 오류 보고가 guest page-table 일관성을 결정한다. |
| P1 | [I/O page-fault delivery](https://lore.kernel.org/all/20240616061155.169343-1-baolu.lu@linux.intel.com/) | Lu Baolu, Yi Liu | v6.11 | device fault를 fault FD로 전달하고 userspace 응답을 IOMMU driver로 되돌리는 비동기 경로다. queue overflow, pending response, HWPT 파괴와 fault FD close가 겹칠 때의 수명을 분석해야 한다. |
| P0 | [`IOMMU_IOAS_MAP_FILE` for memfd](https://lore.kernel.org/all/1729861919-234514-8-git-send-email-steven.sistare@oracle.com/) | Steve Sistare | v6.13 | user VA가 아니라 `fd + offset`을 mapping identity로 사용해 프로세스 주소공간과 backing을 분리한다. 이후 VFIO DMA-BUF와 `guest_memfd` 지원이 이 API를 확장하므로 folio pin, truncate, accounting 및 rollback의 기준 구현이다. |
| P0 | [`IOMMU_IOAS_CHANGE_PROCESS`](https://lore.kernel.org/all/1731527497-16091-4-git-send-email-steven.sistare@oracle.com/) | Steve Sistare | v6.13 | file-backed IOAS를 다시 만들지 않고 pinned-memory accounting을 새 프로세스로 넘긴다. FD handoff 중 old/new `mm`, `RLIMIT_MEMLOCK`, 실패 시 원자성과 non-file mapping 거부 이유를 확인해야 userspace live update 계약을 이해할 수 있다. |
| P1 | [vIOMMU, vDEVICE, SMMUv3 nesting](https://lore.kernel.org/all/cover.1729897352.git.nicolinc@nvidia.com/) | Nicolin Chen | v6.13 | physical DEVICE/HWPT와 VM이 보는 IOMMU/device identity를 분리한 현재 virtualization 객체 모델이다. vIOMMU가 parent HWPT를 공유하는 방법과 vDEVICE의 virtual ID/refcount가 AMD vIOMMU, TSM 및 vEVENTQ의 기반이 된다. |
| P1 | [ARM ITS nested management](https://lore.kernel.org/all/cover.1740014950.git.nicolinc@nvidia.com/) | Nicolin Chen, Robin Murphy | v6.15 | nested translation에서는 guest가 정한 MSI IOVA와 host ITS physical page를 두 stage에 걸쳐 연결해야 한다. reserved MSI region, identity/RMR mapping 및 interrupt remapping 책임이 VMM과 kernel 사이에서 어떻게 나뉘는지 확인해야 한다. |
| P1 | [vEVENTQ](https://lore.kernel.org/all/cover.1737754129.git.nicolinc@nvidia.com/) | Nicolin Chen | v6.15 | nested-stage fault와 vendor event를 physical ID가 아닌 virtual device ID로 userspace에 전달한다. queue depth, sequence, lost-event 표시와 vDEVICE가 사라질 때의 event lifetime이 correctness 핵심이다. |
| P1 | [PASID support](https://lore.kernel.org/all/20231127063428.127436-1-yi.l.liu@intel.com/) | Kevin Tian, Yi Liu | v6.15 | 하나의 physical device 아래 여러 address space를 `{device, PASID}` 단위로 attach/replace/detach한다. RID 단위 group 모델과 다른 isolation, singleton 조건 및 PASID 재사용 시 ordering을 분석해야 SVA/SIOV 확장을 판단할 수 있다. |
| P1 | [`IOMMU_HW_QUEUE_ALLOC`](https://lore.kernel.org/all/cover.1752126748.git.nicolinc@nvidia.com/) | Nicolin Chen | v6.15 | guest queue memory를 hardware command queue에 연결해 VM exit 없이 IOMMU 명령을 처리하게 한다. queue memory pinning, `nesting_parent_iova`, mmap 영역, index 충돌과 vIOMMU 파괴 순서를 확인해야 한다. |
| P0 | [VFIO PCI MMIO DMA-BUF exporter](https://github.com/torvalds/linux/commit/5d74781ebc86c5fa9e9d6934024c505412de9b52) | Jason Gunthorpe, Vivek Kasireddy, Leon Romanovsky | v6.19 | VFIO PCI BAR/MMIO range를 revocable DMA-BUF fd로 만드는 공급자다. device close/reset 시 revoke, device reference, P2P topology와 range 검증이 #38에 전달되는 fd의 안전성을 결정한다. Vivek이 Jason의 초기 작업을 되살렸고 Leon이 후속 series를 정리했다. |
| P0 | [IOMMUFD의 VFIO DMA-BUF `IOAS_MAP_FILE` importer](https://lore.kernel.org/all/8-v2-b2c110338e3f+5c2-iommufd_dmabuf_jgg@nvidia.com/) | Jason Gunthorpe | v6.19 | exporter가 만든 DMA-BUF를 `IOMMU_IOAS_MAP_FILE`로 받아 IOAS에 넣는 반대쪽 절반이다. 현재는 임의 DMA-BUF가 아니라 지원되는 single-range VFIO DMA-BUF에 한정되므로 exporter와 importer의 lifetime 및 revoke 계약을 함께 분석해야 한다. |
| P0 | [Consolidated page table](https://lore.kernel.org/all/0-v5-116c4948af3d+68091-iommu_pt_jgg@nvidia.com/) | Jason Gunthorpe, Alejandro Jimenez | v6.19 | vendor별 page-table walker의 공통 알고리즘을 generic page table로 모아 map/unmap, dirty-bit와 testing을 한곳에서 개선할 수 있게 한다. no-IOMMU software HWPT, live-update 보존 및 향후 batching/cut 최적화가 이 기반을 사용하므로 단순 리팩터로 보면 안 된다. |

### 3.2 진행 중인 시리즈

| 우선순위 | 시리즈 | 대표 author | 현재 확인 상태 | 분석 이유 |
|---|---|---|---|---|
| P0 | [DMA-BUF mapping types와 PAL](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/) | Jason Gunthorpe | RFC v1, 26 patches | 기존 SGT-only attachment 계약을 mapping-type 협상으로 바꾸고 PAL로 physical/MMIO range를 전달한다. CUDA/GPU exporter 적용 가능성뿐 아니라 raw address 신뢰 경계, revoke와 IOMMUFD publish race를 merge된 VFIO DMA-BUF 경로와 비교해야 한다. |
| P0 | [IOMMUFD no-IOMMU cdev](https://lore.kernel.org/all/cover.1783360051.git.jacob.pan@linux.microsoft.com/) | Jacob Pan, Jason Gunthorpe | v10, 6 patches | hardware IOMMU 없이 IOMMUFD가 page pin과 software IOAS를 제공하고 IOVA-to-PA 조회를 허용한다. UPCIE의 UIO backend, `/proc/pagemap`, `phys_lut`을 upstream API로 대체할 수 있는지와 `CAP_SYS_RAWIO` 보안 경계를 직접 판단할 수 있다. |
| P1 | [QEMU multi-range VFIO DMA-BUF와 virtio-gpu](https://patchew.org/QEMU/20260319052023.2088685-1-vivek.kasireddy%40intel.com/) | Vivek Kasireddy | v12, 10 patches | 여러 IOV가 같은 VFIO region에 속하는지 확인해 하나의 DMA-BUF로 만들고 virtio-gpu blob backing으로 사용한다. UPCIE의 직접 dependency는 아니지만 #38의 단일 range 가정을 확장할 때 range 합산, mmap, backing 교체와 cleanup을 검토할 실제 consumer다. |
| P1 | [`guest_memfd`의 `IOAS_MAP_FILE` 매핑](https://lore.kernel.org/all/20260225075211.3353194-1-aik@amd.com/) | Alexey Kardashevskiy | RFC, 1 patch | userspace에 mmap할 수 없는 CoCo private page를 file-backed mapping에 넣는다. KVM이 folio lifetime을 보장한다는 가정, page-state 전환과 shrink 통지 부재를 기존 memfd/PAL 계약과 비교해야 한다. |
| P1 | [AMD nested translation](https://lore.kernel.org/all/20260115060814.10692-1-suravee.suthikulpanit@amd.com/) | Suravee Suthikulpanit | v6, 13 patches | AMD DTE에 host stage-2와 guest stage-1을 결합하는 실제 vendor 구현이다. generic HWPT UAPI가 gDTE validation, domain ID, attach/detach 및 invalidation으로 변환되는 과정을 확인할 수 있다. |
| P1 | [AMD hardware vIOMMU](https://lore.kernel.org/all/20260629153535.15775-1-suravee.suthikulpanit@amd.com/) | Suravee Suthikulpanit | v3, 22 patches | merge된 vIOMMU/vDEVICE/HW queue 추상화를 AMD guest ID, VF MMIO, private-address 영역과 ID translation table에 연결한다. 공통 IOMMUFD 객체가 vendor state를 충분히 표현하는지 검증할 수 있다. |
| P2 | [TSM ioctl과 TDISP device assignment](https://lore.kernel.org/all/20260427061005.901854-1-aneesh.kumar@kernel.org/) | Aneesh Kumar K.V, Nicolin Chen, Shameer Kolothum Thodi | v4, 4 patches | vDEVICE를 KVM/TSM과 연결해 device lockdown, bind/unbind 및 guest attestation request를 처리한다. confidential device assignment의 ownership과 destroy ordering을 `guest_memfd` private DMA 흐름과 함께 봐야 한다. |
| P2 | [IOMMU/VFIO live update](https://lore.kernel.org/all/20260427175633.1978233-1-skhawaja@google.com/) | Samiullah Khawaja, YiFei Zhu | v2, 16 patches | IOMMUFD FD, HWPT, domain, VFIO cdev와 translation-unit 상태를 커널 live update 너머로 보존한다. generic page-table serialization, restored domain 교체, 실패 rollback 및 versioning을 통해 가장 긴 객체 lifetime을 검토할 수 있다. |

P0는 UPCIE의 현재 메모리 및 no-IOMMU 설계와 직접 충돌하거나 이를 대체할 수
있는 작업이다. P1은 IOMMUFD의 메모리 공급자와 vendor 구현을 이해하기 위해
필요하다. P2는 현재 기능의 직접 의존성은 아니지만 객체 lifetime과 향후
confidential-compute 지원을 분석할 때 중요하다.

### 3.3 전체 흐름

IOMMUFD의 출발점은 userspace가 `/dev/iommu`에서 IOAS를 만들고, VFIO 같은
consumer가 넘긴 physical DEVICE를 그 IOAS에 연결하는 것이다. IOAS는 IOVA와
backing page의 관계를 보관하고, HWPT_PAGING은 그 관계를 실제 `iommu_domain`의
page table로 구현한다. 이 기본 경로만으로도 VFIO type1이 내부에 갖고 있던
mapping과 domain 관리를 공통 subsystem으로 옮길 수 있다.

다음 단계에서는 VM이 자신의 IOMMU page table을 직접 관리할 수 있도록
HWPT_NESTED가 추가됐다. guest가 stage-1 PTE를 변경하면 invalidation을 요청해야
하고, device가 translation fault를 내면 fault FD를 통해 VMM이 받아 응답해야
한다. migration을 위해서는 stage-2의 DMA write dirty bit도 회수해야 한다.
따라서 user HWPT, invalidation, fault delivery와 dirty tracking은 각각 떨어진
기능이 아니라 nested translation의 한 상태기계를 구성한다.

그 위에 vIOMMU는 하나의 VM이 보는 IOMMU instance를, vDEVICE는 physical
DEVICE가 그 vIOMMU 안에서 사용하는 virtual ID를 표현한다. PASID는 attachment
단위를 device보다 더 잘게 나누고, vEVENTQ는 hardware event를 virtual ID로
VMM에 전달하며, HW queue는 guest queue를 실제 IOMMU hardware에 직접 연결한다.
ARM ITS와 AMD vIOMMU 작업은 이 추상 객체들이 interrupt, DTE와 command queue
같은 vendor hardware state로 변환되는 사례다.

메모리 공급 경로도 user VA 하나에서 file-backed memory로 확장됐다. v6.13의
`IOMMU_IOAS_MAP_FILE`은 memfd의 `fd + offset`을 mapping identity로 삼아 프로세스
주소공간에서 backing lifetime을 분리했고, `CHANGE_PROCESS`는 이 성질을 이용해
pin accounting을 새 프로세스로 넘긴다. v6.19에서는 VFIO PCI BAR를 DMA-BUF로
export해 같은 ioctl로 매핑하면서, NVMe와 VFIO-bound GPU BAR를 하나의 IOAS에서
만나는 mainline 경로가 완성됐다.

이 v6.19 경로는 한 패치가 아니라 두 subsystem의 결합이다. VFIO exporter는
Jason의 초기 작업을 Vivek Kasireddy가 되살린 뒤 Leon Romanovsky가 정리해
BAR/MMIO range를 revocable DMA-BUF fd로 만든다. IOMMUFD importer는 그 fd를
`IOMMU_IOAS_MAP_FILE`로 받아 IOAS에 게시한다. 따라서 exporter가 없으면
importer에 넘길 fd가 없고, importer가 없으면 export된 BAR를 NVMe와 같은
IOAS에 넣을 수 없다.

현재 RFC들은 이 두 축을 더 확장한다. DMA-BUF mapping types/PAL은 VFIO 전용
single-range를 넘어 importer와 exporter가 physical/MMIO 표현을 협상하려 하고,
`guest_memfd`는 mmap할 수 없는 private memory를 file-backed mapping에 넣으려
한다. no-IOMMU는 page table hardware가 없어도 IOAS를 page-pin/PA-query registry로
사용하려 한다. TSM/TDISP는 이 memory와 vDEVICE를 confidential VM의 attestation
수명에 묶고, live update는 마지막으로 전체 객체와 hardware state를 커널 교체
너머까지 보존하려 한다.

UPCIE 관점에서 흐름을 한 줄로 줄이면 다음과 같다. 먼저 merge된 VFIO DMA-BUF
경로로 VFIO-bound BAR P2P가 가능해졌고, PAL RFC는 vendor driver가 소유한 CUDA
메모리까지 같은 모델로 가져올 가능성을 탐색하며, no-IOMMU 시리즈는 반대편의
UIO/physical-address backend까지 IOMMUFD 객체 모델로 통합하려는 작업이다.

## 4. Merge된 기능에서 먼저 확인할 것

### 4.1 기본 객체와 ownership

IOMMUFD를 분석할 때 먼저 다음 객체 관계를 고정해야 한다.

```text
userspace /dev/iommu context
   |
   +-- IOAS -------------------- memory mappings and IOVA allocator
   |    |
   |    +-- HWPT_PAGING -------- kernel-managed stage-2 iommu_domain
   |         |  |
   |         |  +-- HWPT_NESTED - user/guest-managed stage-1
   |         |
   |         +----- vIOMMU ------ VM-visible IOMMU context
   |                   |
   |                   +-- vEVENTQ / HW_QUEUE
   |
   +-- DEVICE ------------------ physical device bound by VFIO/driver
          |
          +-- vDEVICE ---------- DEVICE identity inside one vIOMMU
```

여기서는 ioctl의 성공 경로보다 객체 생성 순서, 참조 관계, `IOMMU_DESTROY`,
device detach와 FD close가 겹칠 때의 종료 순서를 먼저 분석해야 한다. 이후의
PAL, no-IOMMU, TSM 및 live-update 시리즈가 모두 이 ownership 모델을 확장하기
때문이다.

### 4.2 User HWPT, invalidation과 fault delivery

v6.7~v6.11의 dirty tracking, user HWPT, invalidation 및 I/O page-fault delivery는
nested translation의 선행 기능이다.

- Dirty tracking은 DMA write를 migration bitmap에 반영하는 방법을 정한다.
- User HWPT는 stage-1 page table을 userspace 또는 guest가 소유하게 한다.
- Invalidation은 page-table 수정 이후 IOTLB를 무효화하는 명시적 계약이다.
- Fault object는 I/O page fault를 FD로 전달하고 userspace 응답을 driver로
  돌려보낸다.

이 기능군을 보지 않고 vIOMMU만 분석하면 guest가 관리하는 translation 상태와
커널이 관리하는 stage-2의 책임 경계를 놓치게 된다.

### 4.3 v6.13: file-backed mapping과 process handoff

`IOMMU_IOAS_MAP_FILE`은 user VA 대신 `fd + offset`으로 IOAS mapping을 만든다.
최초 memfd 지원은 이후 VFIO DMA-BUF와 `guest_memfd`가 들어올 수 있는
memory-source 추상화의 기준이 됐다.

`IOMMU_IOAS_CHANGE_PROCESS`는 같은 IOMMUFD context의 file-backed mapping에
대한 pinned-memory accounting을 현재 프로세스로 이전한다. 따라서 두 기능은
별도 ioctl이 아니라 다음 하나의 수명 모델로 같이 분석해야 한다.

```text
file owns backing pages
   -> IOMMUFD pins/maps file range
   -> IOMMUFD FD moves to another process
   -> pin accounting moves without rebuilding IOAS
```

확인할 항목은 file truncation 금지, folio pinning, memlock accounting의 원자성,
부분 실패 rollback 및 이전 프로세스가 사라질 때의 동작이다.

### 4.4 v6.13~v6.15: vIOMMU 기능군

vIOMMU와 vDEVICE는 physical DEVICE/HWPT와 VM이 보는 virtual identity를
분리한다. vEVENTQ, PASID, HW queue 및 ARM ITS 관리는 이 객체 위에 놓인다.

- vDEVICE는 physical device와 guest-visible ID를 연결한다.
- vEVENTQ는 nested-stage fault와 hardware event를 virtual ID로 전달한다.
- PASID는 하나의 device 아래 여러 address-space attachment를 표현한다.
- HW queue는 guest queue memory와 IOMMU hardware queue를 직접 연결한다.
- ARM ITS 관리는 nested IOMMU와 interrupt translation의 일관성을 맞춘다.

분석에서는 queue overflow와 lost-event 표시, virtual ID 변환 실패, PASID
attach/detach ordering, queue memory pinning 및 mmap 수명을 함께 확인해야 한다.

### 4.5 v6.19: VFIO DMA-BUF와 consolidated page table

VFIO DMA-BUF 경로는 exporter와 importer를 분리해 이해해야 한다.

```text
VFIO PCI BAR/MMIO range
  → VFIO_DEVICE_FEATURE_DMA_BUF
  → revocable DMA-BUF fd
  → IOMMU_IOAS_MAP_FILE
  → IOMMUFD IOAS의 IOVA
```

첫 번째 절반은 [Vivek Kasireddy의 v3 series](https://lore.kernel.org/all/20250307052248.405803-1-vivek.kasireddy@intel.com/)가
Jason Gunthorpe의 기존 VFIO exporter 작업을 되살린 계보다. 이후 Leon
Romanovsky가 P2PDMA 정리와 revoke를 포함한 series로 발전시켰고
[핵심 exporter commit](https://github.com/torvalds/linux/commit/5d74781ebc86c5fa9e9d6934024c505412de9b52)이
v6.19에 merge됐다. Vivek이 직접 작성한 VFIO device reference와 feature 호출
helper도 최종 series에 포함됐다.

두 번째 절반은 Jason Gunthorpe의
[IOMMUFD DMA-BUF importer](https://lore.kernel.org/all/8-v2-b2c110338e3f+5c2-iommufd_dmabuf_jgg@nvidia.com/)다.
VFIO가 만든 fd를 `IOMMU_IOAS_MAP_FILE`로 받아 IOAS에 게시한다. 이 경로는
GPU를 vendor driver에 둔 채 CUDA allocation을 export하는 문제와는 다르지만,
VFIO-bound GPU BAR를 NVMe와 같은 IOAS에 배치하는 UPCIE PR #38의 upstream
기반이다.

분석할 항목은 다음과 같다.

- VFIO DMA-BUF가 허용하는 단일 physical range와 page alignment
- DMA-BUF revoke와 IOMMUFD unmap의 동기화
- VFIO device close 및 reset 중 mapping lifetime
- PCI P2P topology와 IOMMU mapping 가능 여부
- 일반 DMA-BUF importer가 아니라 제한된 VFIO DMA-BUF 경로라는 UAPI 범위

Vivek의 진행 중인
[QEMU v12 series](https://patchew.org/QEMU/20260319052023.2088685-1-vivek.kasireddy%40intel.com/)는
이 UAPI를 사용하는 중요한 consumer다. System RAM이면 memfd/udmabuf를,
VFIO device-local memory이면 여러 VFIO range로 만든 DMA-BUF를 virtio-gpu
blob backing으로 사용한다. UPCIE에 QEMU나 virtio-gpu가 필요하다는 뜻은
아니지만, 다음 문제를 이미 다루므로 #38의 후속 설계에 참고할 가치가 크다.

- 여러 IOV가 동일 VFIO region에 속하는지 검증
- range 전체 크기와 DMA-BUF 크기의 일치
- DMA-BUF mmap과 실패 시 fd/object cleanup
- guest backing 변경 시 기존 DMA-BUF 파괴와 재생성

반면 Vivek의
[virtio-gpu 외부 DMA-BUF import](https://lore.kernel.org/dri-devel/20241126031643.3490496-1-vivek.kasireddy@intel.com/)는
VM의 scanout/blob 전달을 위한 작업이다. 가상화까지 범위를 넓힐 때는
중요하지만 현재 host NVMe-to-GPU BAR 경로의 직접 dependency는 아니다.

Consolidated page table은 vendor마다 중복되던 page-table walker를 generic page
table로 모은 작업이다. 자체로 끝나는 리팩터가 아니라 no-IOMMU software HWPT,
live-update page-table 보존, dirty tracking 및 향후 map/unmap 최적화의 기반이므로
별도 분석 가치가 있다.

## 5. DMA-BUF mapping types와 PAL

### 5.1 시리즈 개요

`[PATCH RFC 00/26] Add DMA-buf mapping types and convert vfio/iommufd to
use them`은 DMA-BUF attachment가 항상 `sg_table`을 교환한다는 전제를 없애고,
exporter와 importer가 mapping 표현을 협상하게 만든다.

시리즈는 두 부분으로 나뉜다.

1. 01/26~20/26은 mapping type 협상 API와 기존 SGT 사용자의 전환이다.
2. 21/26~25/26은 PAL(Physical Address List)을 추가하고 VFIO PCI를 exporter,
   IOMMUFD를 importer로 연결한다.

PAL은 CPU physical 또는 MMIO 주소 범위를 전달할 수 있어 GPU VRAM과 PCI BAR를
IOMMUFD IOAS에 연결하려는 UPCIE의 문제와 가장 가깝다. 반면 raw physical
address 공개, 범위 검증, revoke 동기화 및 객체 lifetime에 대한 합의가 아직
부족하다.

### 5.2 현재 판정

- mapping 협상과 SGT 정리는 독립적으로 발전시킬 가치가 크다.
- PAL은 단순 자료구조가 아니라 별도의 신뢰 및 lifetime 모델을 요구한다.
- v1에는 VFIO 객체 lifetime, IOMMUFD publish/revoke 경쟁, PAL walker 검증과
  드라이버별 변환 오류가 남아 있다.
- 전체 v1을 그대로 병합하기보다 협상/SGT와 PAL을 분리하는 것이 적절하다.

패치별 분석은
[DMA_BUF_MAPPING_TYPES_RFC_V1_ANALYSIS.md](DMA_BUF_MAPPING_TYPES_RFC_V1_ANALYSIS.md)에
정리되어 있다.

## 6. IOMMUFD no-IOMMU cdev

### 6.1 시리즈 개요

`[PATCH v10 0/6] iommufd: Enable noiommu mode for cdev`는 하드웨어 IOMMU가
없는 VFIO cdev도 IOMMUFD에 bind하고 IOAS를 사용할 수 있도록 한다. 실제 DMA
remapping은 없지만 IOMMUFD가 페이지를 pin하고 software page table을 관리한다.

새 `IOMMU_IOAS_NOIOMMU_GET_PA` ioctl은 등록된 IOVA에 대응하는 물리 주소와
연속 길이를 userspace에 돌려준다. 이때 IOVA는 장치가 사용하는 주소가 아니라
software mapping을 검색하기 위한 key다.

### 6.2 UPCIE와의 관계

이 시리즈는 UPCIE의 UIO/no-IOMMU backend 및 `phys_lut`과 직접 비교해야 한다.

- `/proc/pagemap` 또는 자체 PA 조회를 upstream ioctl로 바꿀 수 있는가?
- UPCIE의 ARITHMETIC/LUT translator 중 LUT 경로를 단순화할 수 있는가?
- `mlock()`보다 강한 IOMMUFD page pinning을 재사용할 수 있는가?
- `CAP_SYS_RAWIO`, `RLIMIT_MEMLOCK` 및 VFIO ownership 조건이 맞는가?
- IOAS를 공통 메모리 registry로 사용하면 IOMMU와 no-IOMMU backend의 API를
  통합할 수 있는가?

진행 중인 시리즈 중에서는 mapping-types RFC 다음으로 상세 분석할 대상이다.

## 7. `guest_memfd`의 `IOMMU_IOAS_MAP_FILE` 매핑

`[RFC PATCH kernel] iommufd: Allow mapping from KVM's guest_memfd`는 CoCo VM의
private memory를 기존 `IOMMU_IOAS_MAP_FILE`로 매핑하려는 단일 패치다.
`guest_memfd`는 private page를 userspace에 mmap할 수 없으므로 일반
`IOMMU_IOAS_MAP`의 user-VA pinning을 사용할 수 없다.

이 RFC는 DMA-BUF PAL과 다른 file-backed memory 계약을 보여준다.

| 경로 | 메모리 표현 | pin/lifetime 책임 |
|---|---|---|
| 일반 `IOMMU_IOAS_MAP` | userspace VA | IOMMUFD가 GUP/pin 관리 |
| memfd `IOAS_MAP_FILE` | file + offset | file-backed page reader |
| `guest_memfd` RFC | private file + offset | KVM guest_memfd 계약에 의존 |
| DMA-BUF PAL RFC | physical/MMIO range | exporter와 revoke 계약에 의존 |

핵심 검토점은 page-state 변경과 truncate/shrink를 누가 통지하는지, 매핑 중인
folio의 수명을 누가 보장하는지다. RFC는 VMM이 page-state 변경과 memory unplug를
처리한다고 가정하므로, 커널 API가 그 가정을 실제로 강제하는지 확인해야 한다.

## 8. AMD nested translation과 hardware vIOMMU

발표 당시 하나의 `AMD VIOMMU and Nested v5` 항목이었던 작업은 현재 두
시리즈로 추적하는 것이 정확하다.

### 8.1 Nested translation v6

`[PATCH v6 00/13] iommu/amd: Introduce Nested Translation support`는 host가
관리하는 stage-2와 guest가 관리하는 stage-1을 AMD IOMMU DTE에 결합한다.
IOMMUFD의 nest-parent HWPT, nested domain 및 AMD hardware-info UAPI가 실제
vendor driver에 연결되는 사례다.

### 8.2 Hardware-accelerated vIOMMU v3

`[PATCH v3 00/22] iommu/amd: Introduce AMD Hardware-accelerated Virtualized
IOMMU (vIOMMU) Support`는 다음 요소를 추가한다.

- IOMMUFD vIOMMU와 vDEVICE 구현
- guest ID, device ID 및 domain ID 변환
- vIOMMU VF MMIO와 private-address 영역
- guest command/event 처리에 필요한 hardware queue

두 시리즈는 UPCIE의 당장 필요한 GPU 메모리 매핑 API는 아니지만, IOMMUFD의
추상 객체가 실제 하드웨어 상태와 어떻게 결합되는지 이해하기 위한 최신
reference implementation이다.

## 9. TSM/TDISP와 confidential device assignment

`[PATCH v4 0/4] Add iommufd ioctls to support TSM operations`는 VMM이 IOMMUFD를
통해 TSM bind/unbind와 guest request를 수행하게 한다. IOMMUFD의 vIOMMU와
vDEVICE가 KVM VM, TSM core 및 PCI TDISP device의 연결점이 된다.

분석할 핵심은 다음과 같다.

- KVM 객체와 IOMMUFD device/vIOMMU의 연결 수명
- device lockdown, bind, attestation 및 unbind 순서
- `IOMMU_DESTROY`와 TSM-bound vDEVICE의 상호작용
- private memory DMA를 위한 `guest_memfd` 매핑과의 결합

따라서 confidential compute는 하나의 독립 기능이 아니라 `guest_memfd`,
vIOMMU/vDEVICE, TSM/TDISP를 함께 봐야 하는 기능군이다.

## 10. IOMMU/VFIO live update

초기 `[RFC PATCH v2 00/32] Add live update state preservation`은 IOMMUFD FD,
HWPT, IOMMU domain, VFIO cdev와 translation unit 상태를 live update 전후로
보존하려는 설계였다. 이후 IOMMU/VFIO 부분은
`[PATCH v2 00/16] iommu: Add live update state preservation`으로 재구성됐다.

현재 UPCIE 기능의 직접 의존성은 아니지만 다음 이유로 분석 가치가 있다.

- IOMMUFD object와 실제 `iommu_domain`의 lifetime 경계를 드러낸다.
- generic page table을 보존하고 복구하는 방식을 보여준다.
- device detach/reattach와 restored domain 교체 순서를 다룬다.
- VFIO cdev와 IOMMUFD FD의 ownership을 프로세스 재시작보다 긴 수명으로
  확장한다.

전체 코드보다 preserve/unpreserve/restore 상태기계와 실패 rollback을 중심으로
보는 것이 효율적이다.

## 11. 분석 순서

권장 순서는 다음과 같다.

1. IOMMUFD 기본 IOAS/HWPT/DEVICE 객체와 VFIO cdev ownership을 정리한다.
2. v6.13 `MAP_FILE`/`CHANGE_PROCESS`와 v6.19 VFIO DMA-BUF 경로를 이어서
   분석한다.
3. consolidated page table이 mapping과 dirty tracking을 어떻게 구현하는지
   확인한다.
4. DMA-BUF mapping types/PAL을 merge된 VFIO DMA-BUF 경로와 비교하고 후속
   revision을 추적한다.
5. no-IOMMU cdev v10을 UPCIE의 UIO backend 및 `phys_lut`과 비교한다.
6. `guest_memfd` RFC로 `IOAS_MAP_FILE`의 memory-source 및 lifetime 계약을
   비교한다.
7. user HWPT/fault delivery를 바탕으로 AMD nested v6와 hardware vIOMMU v3의
   객체 연결을 확인한다.
8. TSM/TDISP와 `guest_memfd`를 묶어 confidential device assignment 흐름을
   분석한다.
9. live-update 시리즈로 IOMMUFD/HWPT/VFIO의 장기 lifetime 모델을 보완한다.

각 상세 분석 문서는 한 시리즈를 한 문서로 다루고, 이 문서는 버전과 판정만
갱신하는 인덱스로 유지한다.

## 12. 결론

분석은 merge된 기능과 진행 중인 시리즈를 분리하되 둘 중 하나를 생략해서는
안 된다. merge된 기능은 현재 커널이 보장하는 객체, UAPI 및 lifetime 계약이고,
RFC는 그 계약을 확장하거나 바꾸려는 제안이다. 특히 PAL과 `guest_memfd`를
평가하려면 먼저 merge된 `IOMMU_IOAS_MAP_FILE`과 VFIO DMA-BUF 경로를 기준선으로
삼아야 한다. 이때 VFIO exporter와 IOMMUFD importer를 하나의 기능으로 뭉개면
fd 생성 책임과 IOAS mapping 책임, 각자의 revoke lifetime을 구분할 수 없다.

LPC 2025 발표 이후 가장 중요한 변화는 no-IOMMU가 v10까지 구체화됐고, AMD의
nested translation과 hardware vIOMMU가 별도 최신 시리즈로 발전했으며,
confidential compute가 `guest_memfd`와 TSM ioctl이라는 실제 IOMMUFD API로
나타났다는 점이다.

UPCIE에는 다음 두 축이 가장 중요하다.

1. DMA-BUF mapping types/PAL이 CUDA 또는 GPU-exported memory를 IOMMUFD에
   안전하게 전달할 수 있는가?
2. no-IOMMU cdev가 UPCIE의 자체 PA lookup과 backend 분기를 upstream API로
   대체할 수 있는가?

따라서 다음 상세 분석은 merge된 `MAP_FILE`, Vivek/Leon/Jason 계보의 VFIO
DMA-BUF exporter와 Jason의 IOMMUFD importer를 먼저 기준선으로 정리해야 한다.
그다음 QEMU multi-range consumer로 단일 range 가정을 검토하고, no-IOMMU cdev
v10과 `guest_memfd` RFC를 각각 비교하는 순서가 가장 효율적이다.

## 13. 참고 링크

- [LPC 2025 IOMMUFD 발표자료](https://lpc.events/event/19/contributions/2126/attachments/1913/4469/lpc25-gunthorpe.pdf)
- [Linux v6.13 IOMMUFD 문서](https://www.kernel.org/doc/html/v6.13/userspace-api/iommufd.html)
- [Linux v6.15 IOMMUFD 문서](https://www.kernel.org/doc/html/v6.15/userspace-api/iommufd.html)
- [Linux v6.19 IOMMUFD 문서](https://www.kernel.org/doc/html/v6.19/userspace-api/iommufd.html)
- [IOMMUFD base infrastructure commit](https://github.com/torvalds/linux/commit/2ff4bed7fee7)
- [IOMMUFD dirty tracking v6](https://lore.kernel.org/all/20231024135109.73787-1-joao.m.martins@oracle.com/)
- [User HWPT invalidation](https://lore.kernel.org/all/20230511143844.22693-8-yi.l.liu@intel.com/)
- [I/O page-fault delivery v7](https://lore.kernel.org/all/20240616061155.169343-1-baolu.lu@linux.intel.com/)
- [`IOMMU_IOAS_MAP_FILE` patch](https://lore.kernel.org/all/1729861919-234514-8-git-send-email-steven.sistare@oracle.com/)
- [`IOMMU_IOAS_CHANGE_PROCESS` patch](https://lore.kernel.org/all/1731527497-16091-4-git-send-email-steven.sistare@oracle.com/)
- [vIOMMU infrastructure Part 1 v5](https://lore.kernel.org/all/cover.1729897352.git.nicolinc@nvidia.com/)
- [ARM ITS nested management](https://lore.kernel.org/all/cover.1740014950.git.nicolinc@nvidia.com/)
- [vEVENTQ v6](https://lore.kernel.org/all/cover.1737754129.git.nicolinc@nvidia.com/)
- [PASID attachment series](https://lore.kernel.org/all/20231127063428.127436-1-yi.l.liu@intel.com/)
- [`IOMMU_HW_QUEUE_ALLOC` v9](https://lore.kernel.org/all/cover.1752126748.git.nicolinc@nvidia.com/)
- [Vivek의 VFIO PCI MMIO DMA-BUF exporter v3](https://lore.kernel.org/all/20250307052248.405803-1-vivek.kasireddy@intel.com/)
- [Merge된 VFIO PCI MMIO DMA-BUF exporter](https://github.com/torvalds/linux/commit/5d74781ebc86c5fa9e9d6934024c505412de9b52)
- [VFIO DMA-BUF `IOAS_MAP_FILE` patch](https://lore.kernel.org/all/8-v2-b2c110338e3f+5c2-iommufd_dmabuf_jgg@nvidia.com/)
- [QEMU multi-range VFIO DMA-BUF v12](https://patchew.org/QEMU/20260319052023.2088685-1-vivek.kasireddy%40intel.com/)
- [virtio-gpu external DMA-BUF import](https://lore.kernel.org/dri-devel/20241126031643.3490496-1-vivek.kasireddy@intel.com/)
- [Consolidated page table v5](https://lore.kernel.org/all/0-v5-116c4948af3d+68091-iommu_pt_jgg@nvidia.com/)
- [DMA-BUF mapping types RFC v1](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)
- [no-IOMMU cdev v10](https://lore.kernel.org/all/cover.1783360051.git.jacob.pan@linux.microsoft.com/)
- [`guest_memfd` mapping RFC](https://lore.kernel.org/all/20260225075211.3353194-1-aik@amd.com/)
- [AMD nested translation v6](https://lore.kernel.org/all/20260115060814.10692-1-suravee.suthikulpanit@amd.com/)
- [AMD hardware vIOMMU v3](https://lore.kernel.org/all/20260629153535.15775-1-suravee.suthikulpanit@amd.com/)
- [IOMMU live update RFC v2](https://lore.kernel.org/all/20251202230303.1017519-1-skhawaja@google.com/)
- [IOMMU live update v2](https://lore.kernel.org/all/20260427175633.1978233-1-skhawaja@google.com/)
- [TSM ioctl v4](https://lore.kernel.org/all/20260427061005.901854-1-aneesh.kumar@kernel.org/)
- [ARM ITS direct routing RFCv2](https://lore.kernel.org/all/cover.1736550979.git.nicolinc@nvidia.com/)
