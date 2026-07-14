# IOMMUFD에서 SG list만으로 부족하고 PAL이 필요한 이유

## 요약

Linux의 SG(Scatter-Gather) list는 흩어진 메모리를 `struct page`, page 내부 offset과 길이의 목록으로 표현하고, DMA API를 거쳐 특정 장치가 사용할 DMA 주소를 제공한다. 일반적인 DMA-BUF importer에는 이 모델이 적합하지만, IOMMUFD는 특정 장치의 DMA를 수행하는 driver가 아니라 여러 장치가 공유할 IOAS와 IOMMU page table을 직접 구성하는 주체다. 따라서 이미 한 장치에 맞게 변환된 SG DMA 주소보다, IOAS에 새로 매핑할 원본 physical/MMIO range가 필요하다. PAL(Physical Address List)은 exporter가 이 range와 lifetime을 명시적으로 제공하도록 만드는 DMA-BUF mapping 계약이다.

## SG list의 역할과 한계

SG entry에는 별도의 physical-address 필드가 저장되지 않는다. 일반적으로 `struct page`, offset과 length가 backing을 나타내며, `sg_phys()`는 page의 물리주소에 offset을 더해 주소를 계산한다. DMA mapping 후의 `sg_dma_address()`는 이와 다른 값으로, attachment에 지정한 특정 장치가 사용할 IOVA 또는 bus address다.

```text
SG entry
  page + offset + length
            │
            ├─ sg_phys()        → 계산된 physical address
            └─ DMA API mapping  → 특정 장치용 sg_dma_address()
```

일반 GPU나 RDMA importer는 `dma_buf_attach(dmabuf, device)`로 대상 장치를 정한 뒤 그 장치에 맞게 매핑된 SG table을 받으면 된다. 이 과정에서는 exporter가 backing을 관리하고 DMA core가 IOMMU와 PCI P2P 조건을 처리하므로, importer가 raw physical address를 직접 해석할 필요가 없다.

그러나 IOMMUFD는 하나의 attachment device를 위한 DMA 주소가 필요한 것이 아니다. 하나의 IOAS에 NVMe, GPU와 NIC 등 여러 장치를 attach하고, backing을 각 HWPT에 매핑해 공통 IOVA를 만들어야 한다. SG table을 얻기 위해 먼저 어느 장치를 기준으로 `dma_map_sgtable()`을 호출하면, 그 결과는 해당 장치의 기존 DMA domain에 종속된다. 반면 IOMMUFD가 수행하려는 일은 바로 그 domain과 page table을 새로 구성하는 것이다. 결과적으로 “page table을 만들기 위해 이미 page table을 거친 DMA 주소를 요청하는” 순환이 생긴다.

또한 SG list에서 `sg_phys()`를 호출할 수 있다고 해서 이를 generic한 해결책으로 쓸 수는 없다. 모든 DMA-BUF가 일반 system RAM의 `struct page`로 표현되는 것은 아니며, GPU VRAM, PCI BAR/MMIO와 device-private memory는 주소 의미와 cacheability가 다르다. DMA-BUF의 기존 SGT 계약도 importer가 page 정보를 raw physical range로 재해석하도록 보장하지 않는다. `orig_nents`와 DMA mapping 후 병합된 `nents`를 혼동하면 range의 개수와 길이도 달라진다.

특히 일반적인 PCI BAR에는 CPU physical address 공간상의 MMIO 주소는 있지만, 그 주소에 대응하는 일반 RAM의 `struct page`가 없다. SG entry는 보통 `struct page`, offset과 length로 backing을 표현하므로 넣을 page가 없다면 `sg_phys()`로 BAR 주소를 복원하는 page 기반 표현도 사용할 수 없다. PAL은 BAR를 `physical/MMIO base + length`로 직접 기술하기 때문에 `struct page`를 요구하지 않는다.

PCI P2PDMA는 예외다. driver가 적합한 BAR 범위를 P2PDMA resource로 등록하면 커널이 `ZONE_DEVICE` 계열의 특수한 `struct page`를 만들어 SG list로 전달할 수 있다. 그러나 모든 BAR가 이렇게 등록되는 것은 아니며, 특수 page가 존재한다고 해서 모든 장치가 그 BAR까지 DMA할 수 있다는 뜻도 아니다. 실제 접근 가능 여부는 PCI topology와 ACS, root complex의 routing 조건 등에 좌우된다. 따라서 `sg_phys()`로 숫자를 얻을 수 있는지와 그 주소가 IOMMUFD mapping의 유효한 backing인지 여부는 별도로 판단해야 한다.

## udmabuf-import는 어떻게 SG list에서 PA를 얻는가

UPCIE가 사용하는 `udmabuf-import` 확장은 일반 upstream udmabuf의 memfd export 경로와 다르다. `UDMABUF_ATTACH`로 GPU DMA-BUF를 udmabuf misc device에 attach하고 `dma_buf_map_attachment_unlocked()`로 SG table을 받은 뒤, `UDMABUF_GET_MAP`에서 각 entry의 `sg_dma_address()`와 `sg_dma_len()`을 userspace에 반환한다. 즉 `struct page`를 `sg_phys()`로 변환하는 것이 아니라 DMA-BUF exporter와 DMA API가 해당 attachment device에 대해 만든 DMA 주소를 읽는다.

```text
GPU DMA-BUF
  → udmabuf misc device에 attach
  → DMA-BUF exporter가 SG table mapping
  → sg_dma_address() 수집
  → UDMABUF_GET_MAP으로 phys_lut[] 반환
```

`sg_dma_address()`가 PA처럼 사용되는 이유는 udmabuf misc device의 DMA 문맥에 있다. 이 device는 PCI 장치처럼 특정 IOMMU domain에 attach되지 않아 현재 경로에서는 direct-DMA mapping을 사용한다. 따라서 IOMMU가 만든 별도 IOVA가 아니라 GPU BAR를 가리키는 bus/physical 계열 주소가 SG DMA address에 나타나며, UPCIE는 이를 page별 `phys_lut`으로 사용한다. 반대로 IOMMU domain에 속한 장치를 attachment 대상으로 사용했다면 같은 필드는 그 장치 domain의 IOVA가 될 수 있다.

이 방법은 DMA-BUF API가 `sg_dma_address()`를 PA라고 보장해서 성립하는 것이 아니다. direct-DMA 경로도 architecture에 따라 `phys_to_dma()` 변환을 적용할 수 있어 CPU physical address와 DMA bus address가 항상 같지는 않다. 또한 이 값만으로 P2P routing, backing lifetime과 revoke가 보장되지 않는다. 따라서 `phys_lut`은 GPU BAR resource 주소와 대조하고 실제 대상 장치의 접근 가능성을 별도로 검증해야 한다. PAL은 이러한 환경 의존적 추출 대신 exporter가 physical/MMIO range라는 주소 의미와 lifetime을 명시적으로 계약하게 한다.

## PAL이 제공하는 계약

PAL은 backing을 하나 이상의 physical 또는 MMIO range로 명시한다.

```text
PAL
  range[0]: physical/MMIO base A, length
  range[1]: physical/MMIO base B, length
                     │
                     ▼
                  IOMMUFD
                     │
                     ▼
             연속된 IOAS IOVA
```

exporter는 raw range를 제공할 수 있고 그 주소가 유효한 동안 backing을 고정할 책임을 진다. IOMMUFD importer는 range를 검증한 뒤 자신의 IOAS와 HWPT에 매핑한다. physical range가 여러 개로 흩어져 있어도 IOMMUFD가 연속 IOVA에 배치하면 userspace는 `iova_base + offset`의 산술 변환을 계속 사용할 수 있다. 따라서 PAL의 multi-range는 Simon PR #40의 no-IOMMU physical LUT와 다르다. PR #40의 UIO 경로는 `iommu=off` 또는 identity/passthrough 환경에서 장치가 PA와 동일한 DMA 주소를 사용한다고 가정한다. 흩어진 physical page를 하나의 연속 IOVA로 재배치하지 않으므로 fragmentation이 physical LUT에 그대로 노출된다. 반면 PAL 경로에서는 IOMMUFD가 각 physical range를 연속된 IOAS IOVA에 매핑해 fragmentation을 숨길 수 있다.

VFIO GPU BAR를 예로 들면 전체 흐름은 다음과 같다.

```text
VFIO GPU BAR
  → DMA-BUF export
  → PAL로 BAR의 physical/MMIO range 제공
  → IOMMUFD가 IOAS의 IOVA에 매핑
  → NVMe PRP에는 physical address가 아닌 IOVA 기록
```

이 모델은 IOMMUFD가 VFIO 전용 private callback 없이 DMA-BUF의 표준 mapping 협상을 통해 range를 받을 수 있게 한다. 현재 mainline v6.19 경로는 지원되는 single-range VFIO DMA-BUF에 한정된 전용 연결이며, PAL RFC는 이를 generic하고 multi-range인 계약으로 확장하려는 시도다.

## PAL이 해결하지 못하는 문제

PAL은 SGT보다 항상 우수한 표현이 아니다. raw physical/MMIO 주소를 importer에 노출하므로 오히려 더 강한 검증과 lifetime 규칙이 필요하다. importer는 빈 목록, zero-length, page alignment, range overlap, 전체 길이와 DMA-BUF 크기의 일치 및 `base + length` overflow를 확인해야 한다. exporter는 memory 이동, device reset 또는 DMA-BUF release 전에 새 사용자를 막고, 기존 IOMMU mapping과 IOTLB flush가 끝난 뒤 range를 해제해야 한다.

또한 PAL ABI와 IOMMUFD importer가 merge되더라도 CUDA allocation이 자동으로 지원되지는 않는다. NVIDIA 같은 vendor driver가 해당 allocation을 DMA-BUF로 export하고, 안정된 PAL과 pin/revoke 계약을 제공해야 한다. PAL은 exporter가 없는 메모리를 새로 export해 주는 기능이 아니라, exporter와 IOMMUFD 사이의 주소 표현 방식을 정의한다.

## 결론

SG list는 “특정 장치가 바로 DMA할 수 있도록 변환된 주소 목록”에 적합하다. PAL은 “IOMMUFD가 자체 IOAS와 page table을 만들 수 있도록 exporter가 명시한 원본 physical/MMIO range 목록”이다. system RAM처럼 `struct page`가 있으면 `sg_phys()`로 물리주소를 계산할 수 있지만, address semantics·P2P 가능 여부·backing 이동과 revoke lifetime은 여전히 계약되지 않는다. GPU VRAM이나 PCI BAR/MMIO는 일반 RAM의 `struct page`가 없는 경우가 많고, `ZONE_DEVICE`나 P2PDMA의 특수 page가 있더라도 기존 SGT 계약만으로 그 주소를 IOMMUFD용 raw range로 해석해도 된다고 보장할 수 없다. 또한 `sg_dma_address()`는 attachment device 기준으로 mapping된 주소이므로 새 IOAS page table의 raw physical/MMIO range로 일반화할 수 없다. 따라서 IOMMUFD에 PAL이 필요한 이유는 두 겹이다. 기존 SGT 계약에는 MMIO/VRAM의 raw range를 요구하는 표준 수단이 없고, 주소를 얻을 수 있는 경우조차 누가 제공하고 검증하며 언제까지 유효하게 유지하는지를 명확히 해야 하기 때문이다.

관련 patch는 [PAL type 추가 21/26](https://lore.kernel.org/all/21-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/), [VFIO PAL exporter 22/26](https://lore.kernel.org/all/22-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/), [IOMMUFD PAL importer 23/26](https://lore.kernel.org/all/23-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)와 [multi-range 지원 24/26](https://lore.kernel.org/all/24-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)이다.
