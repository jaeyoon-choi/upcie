# DMA-BUF mapping types RFC v1 분석

> 분석 대상: `[PATCH RFC 00/26] Add DMA-buf mapping types and convert
> vfio/iommufd to use them`
>
> 원문 스레드:
> <https://lore.kernel.org/linux-media/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/T/#t>
>
> 기준일: 2026-07-10

## 1. 요약

이 RFC는 DMA-BUF attachment가 항상 `sg_table`을 교환한다는 기존 전제를
없애고, exporter와 importer가 attachment 시점에 사용할 mapping 표현을
협상하도록 만드는 26개 패치의 v1 시리즈다. 첫 mapping type은 기존 동작을
정식 모델로 옮긴 SGT이고, 두 번째는 CPU physical/MMIO 주소 범위를 직접
표현하는 PAL(Physical Address List)이다. VFIO PCI가 PAL exporter가 되고
iommufd가 PAL importer가 되면서, 기존의 VFIO 전용 symbol 연결을 일반적인
DMA-BUF attachment로 대체하려는 것이 최종 목표다.

시리즈의 핵심 아이디어는 두 부분으로 나눠 평가해야 한다.

1. **mapping type 협상과 SGT 전환(01/26~20/26)**

   기존 DMA-BUF API가 암묵적으로 가정하던 SGT 계약을 명시적인 mapping
   type으로 만들고 향후 다른 표현을 추가할 수 있게 한다. exporter가 후보
   우선순위를 통제하고 attachment가 선택 결과를 보존하는 구조는 설계적으로
   유용하다. 다만 v1에는 NULL 처리, 중간 패치의 bisectability, 드라이버별
   변환 버그가 남아 있다.

2. **PAL과 VFIO/iommufd 전환(21/26~25/26)**

   raw physical address를 importer에 노출하는 안전성 모델에 유지보수자 합의가
   없다. range 검증, revoke 동기화, lifetime, symbol 제한 우회 같은 코드 문제도
   존재한다. 현재 형태의 PAL importer API는 별도 재설계 없이는 병합되기 어렵다.

따라서 이 문서의 최종 판정은 다음과 같다.

- 협상 프레임워크와 SGT 정리는 살릴 가치가 크다.
- 링크된 v1 전체는 그대로 병합할 수 없다.
- 특히 AMDGPU 반환값, VFIO lifetime, iommufd revoke 경쟁, PAL walker 검증은
  correctness blocker다.
- 현실적인 v2는 SGT 기반부와 PAL/iommufd 부분을 분리하고, PAL을 core 내부
  converter 또는 exporter-driven IOMMU mapping 형태로 다시 설계해야 한다.

## 2. 시리즈 메타데이터

| 항목 | 값 |
|---|---|
| 작성자 | Jason Gunthorpe |
| 게시일 | 2026-02-17 |
| 패치 수 | 26 |
| 변경 규모 | 55 files, `+1764/-614` |
| base commit | `c63e5a50e1dd291cd95b04291b028fdcaba4c534` |
| 개발 브랜치 | <https://github.com/jgunthorpe/linux/commits/dmabuf_map_type> |
| 새 테스트 파일 | `drivers/dma-buf/st-dma-mapping.c` |

이 문서는 cover letter, 개별 패치, 공개 리뷰 스레드를 기준으로 작성했다.
코드에서 직접 확인되는 사항과 리뷰어의 주장, 이 문서의 추론은 가능한 한
구분해서 표현한다.

## 3. 기존 DMA-BUF 모델의 구조적 문제

### 3.1 기존 attachment 흐름

기존 DMA-BUF의 대표적인 흐름은 다음과 같다.

```text
importer
   |
   | dma_buf_attach(dmabuf, dev)
   v
attachment -- dev, allow_peer2peer, importer_ops
   |
   | dma_buf_map_attachment(direction)
   v
exporter -> sg_table -> importer
```

API의 표면만 보면 exporter가 단순히 scatter-gather 목록을 반환하는 것처럼
보이지만, 그 안의 주소 의미는 exporter와 attachment device, DMA API 호출
여부에 따라 달라진다. `sg_table`에는 CPU physical 주소, DMA address, PCI P2P
주소가 겹쳐 표현될 수 있다. importer가 그 의미를 잘못 해석하면 단순 API
오용을 넘어 잘못된 장치 접근이나 데이터 손상이 발생한다.

### 3.2 SGT 하나로 표현하기 어려운 사례

이 RFC가 문제로 드는 대표 사례는 다음과 같다.

- VFIO가 iommufd/KVM에 전달해야 하는 CPU physical 또는 MMIO 주소
- Xe vGPU PF 내부 VRAM 주소처럼 일반 DMA API 의미와 다른 주소
- UALink 등 Linux DMA API를 거치지 않는 주소 공간
- importer가 이미 IOMMU domain을 관리하여 exporter의 DMA mapping을 원하지
  않는 경우
- 장기적으로 `scatterlist` 자체를 다른 range 표현으로 교체하려는 경우

핵심 문제는 자료구조의 모양만이 아니다. 주소의 **종류**, mapping의
**소유권**, cache coherency 책임, revoke와 unmap 순서를 API 계약이 명시하지
않는다는 점이다.

### 3.3 VFIO/iommufd의 기존 private bridge

VFIO PCI dma-buf를 iommufd에 연결하기 위해 기존 코드는 일반 DMA-BUF API만으로
표현하기 어려운 physical vector를 별도 symbol과 동적 symbol lookup으로
전달한다. 이 연결은 다음 문제를 만든다.

- VFIO와 iommufd 사이에 숨은 결합이 생긴다.
- DMA-BUF core가 attachment의 실제 mapping 종류를 알지 못한다.
- lifetime과 revoke 계약이 subsystem별 코드에 흩어진다.
- selftest도 전용 hook을 흉내 내야 한다.

이 RFC는 mapping 종류를 core에서 협상하여 이 private bridge를 제거하려 한다.

## 4. 새 mapping type 협상 모델

### 4.1 구성 요소

개념적으로 새 API는 다음 네 요소로 구성된다.

- **mapping type**: 주소 표현과 map/unmap 계약을 식별하는 singleton 객체
- **exporter offer**: exporter가 제공할 수 있는 mapping 후보와 우선순위
- **importer match**: importer가 받을 수 있는 type과 추가 조건
- **attachment result**: 최종 선택된 type과 협상 데이터를 저장하는 공간

type 일치는 문자열이나 enum이 아니라 singleton 객체의 포인터 동일성으로
판단한다. 모듈 전체에서 동일한 type 객체를 공유한다는 전제가 ABI 역할을 한다.

### 4.2 협상 순서

협상은 exporter 후보를 바깥쪽 루프로, importer 후보를 안쪽 루프로 순회한다.

```text
for each exporter offer in exporter preference order:
    for each importer match in importer preference order:
        if type pointers differ:
            continue

        result = type-specific match(offer, match)

        if result == -EOPNOTSUPP:
            continue
        if result is another error:
            abort attachment

        finish_match(attachment, offer, match)
        return success

return -EOPNOTSUPP
```

중요한 의미는 다음과 같다.

- **exporter 우선**: importer가 후보를 어떤 순서로 내더라도 exporter의 첫 번째
  호환 offer가 선택된다.
- **`-EOPNOTSUPP`는 soft mismatch**: 현재 조합만 맞지 않으므로 다음 후보를
  시도한다.
- **다른 errno는 fatal**: 자원 부족이나 내부 오류를 호환성 실패로 숨기지 않고
  attachment 전체를 중단한다.
- **선택은 attachment lifetime 동안 고정**: `finish_match()`가 결과를
  `attach->map_type`에 저장하고 이후 map/unmap/debug 경로가 이를 사용한다.

### 4.3 exporter-first의 장단점

exporter-first는 실제 backing과 제약을 가장 잘 아는 exporter가 안전한 표현을
선호할 수 있다는 장점이 있다. 예를 들어 P2P가 가능할 때 exporter가 P2P SGT를
일반 SGT보다 먼저 제안할 수 있다.

반면 importer의 첫 선택이 보장되지 않는다는 점은 API 문서에 매우 명확히
기록해야 한다. importer 후보 순서는 같은 exporter offer에 대응하는 여러
조건 사이에서만 의미가 있다. 이 점을 모르면 importer가 자신의 배열 순서가
전역 우선순위라고 오해할 수 있다.

### 4.4 `finish_match()` 계약

`finish_match()`는 단순 후처리 callback이 아니다. type-specific 협상 결과를
generic attachment에 영구 반영하는 사실상 필수 단계다. 따라서 v2에서는
다음을 명시해야 한다.

- callback이 필수인지 optional인지
- 실패할 수 있는지, 실패한다면 누가 부분 상태를 정리하는지
- callback 전후 attachment에서 읽을 수 있는 필드
- exporter private object의 reference가 언제 획득되는지
- detach/revoke와 어떤 lock 또는 ordering으로 직렬화되는지

v1은 이 계약이 코드 구조에 비해 문서화가 부족하다.

## 5. SGT mapping type

### 5.1 목적

SGT mapping type은 기존 `map_dma_buf()`/`unmap_dma_buf()` 동작을 새 협상 모델로
옮기는 호환성 기반이다. 구 API는 SGT match를 구성하여 generic mapping attach를
호출하는 wrapper가 되고, exporter들은 SGT exporter ops를 제공하도록 변환된다.

### 5.2 일반 SGT와 P2P SGT

SGT 협상에는 다음 조건이 들어간다.

- exporter가 일반 DMA mapping을 제공하는가
- exporter가 P2P만 요구하는가
- importer가 peer-to-peer를 허용하는가
- exporter PCI device와 importer device 사이의 P2P distance가 유효한가
- 특정 backing이 이동 가능하거나 특정 압축 형식을 사용하는가

P2P 적합성 판단을 core로 옮기면 각 exporter가 `pci_p2pdma_distance()`를
제각각 호출하는 중복을 줄일 수 있다. 그러나 importer 요구와 exporter 능력을
한 enum/데이터 구조에 섞으면 유효하지 않은 조합이 생길 수 있으므로 역할별
타입 구분이나 엄격한 validation이 필요하다.

### 5.3 attachment device 접근 제한

변환 이후 exporter가 generic `attach->dev`를 직접 읽는 대신 협상된 SGT ops의
accessor를 사용한다. 이는 non-SGT attachment에는 DMA device 개념이 없을 수
있다는 설계 의도를 코드에 반영한다.

## 6. PAL mapping type

### 6.1 표현

PAL은 하나 이상의 physical/MMIO range를 표현한다.

```text
PAL
 +-- range[0]: physical base, length
 +-- range[1]: physical base, length
 +-- ...
```

iommufd importer는 요청한 offset과 length를 이 range 목록 위에서 순회하고,
각 구간을 PFN batch로 바꾸어 자신의 mapping 경로에 공급한다.

### 6.2 반드시 정의되어야 할 invariant

v1에는 다음 조건의 문서화와 강제 검증이 충분하지 않다.

- 목록이 비어 있어도 되는가
- range 길이가 0일 수 있는가
- base와 length가 PAGE_SIZE에 정렬되어야 하는가
- `base + length` overflow를 어떻게 처리하는가
- range가 서로 겹쳐도 되는가
- 전체 range 길이가 dma-buf size와 정확히 같아야 하는가
- 요청 offset/length를 목록이 완전히 덮지 못할 때의 errno는 무엇인가
- MMIO와 system RAM을 같은 entry로 표현해도 되는가
- cacheability/coherency 속성은 어디에 기록되는가

이 조건들은 최적화나 방어적 코딩 문제가 아니다. PAL을 generic importer API로
노출하려면 주소의 의미와 신뢰 경계를 구성하는 핵심 계약이다.

### 6.3 raw address 공개의 위험

DRM 유지보수자 반대의 핵심은 importer가 exporter backing의 raw physical
address를 직접 받아 해석하게 된다는 점이다. importer가 주소 공간의 종류,
cache 속성 또는 이동 가능성을 잘못 판단하면 exporter가 내부적으로 보장하던
안전 조건을 우회할 수 있다.

symbol export를 `iommufd` 모듈로 제한하더라도 public ops 객체의 function
pointer를 통해 callback을 간접 호출할 수 있다면 제한의 의도가 약해질 수 있다.
보안 모델을 symbol namespace만으로 구성하지 말고 type 접근성과 호출 경계를
함께 검토해야 한다.

## 7. 주요 데이터 흐름

### 7.1 VFIO PCI에서 iommufd로 PAL 전달

```text
VFIO PCI dma-buf exporter
   |
   | offers: PAL first, P2P SGT fallback
   v
DMA-BUF mapping negotiation
   |
   | iommufd importer accepts PAL
   v
PAL attachment
   |
   | map -> physical ranges
   v
iommufd PFN reader -> I/O page table mapping
```

의도대로라면 iommufd는 VFIO 전용 symbol을 알 필요가 없고, VFIO도 iommufd의
내부 API를 직접 호출하지 않는다. 하지만 이 분리는 PAL의 lifetime과 revoke가
DMA-BUF attachment 계약 안에서 완전하게 해결될 때만 성립한다.

### 7.2 P2P SGT fallback

PAL을 받지 않는 기존 importer에는 VFIO가 P2P-only SGT를 fallback으로 제공한다.
이때 importer의 P2P 허용과 PCI distance가 모두 충족되어야 한다.

```text
PAL match 실패(-EOPNOTSUPP)
       |
       v
P2P SGT offer
       |
       +-- importer disallows P2P -> no match
       |
       +-- invalid PCI distance   -> no match
       |
       `-- valid                 -> SGT selected
```

### 7.3 revoke가 개입하는 경우

정상적인 수명 순서는 대략 다음과 같아야 한다.

```text
publish exporter state
  -> attach
  -> map
  -> use mappings
  -> stop new users
  -> revoke/unmap existing users
  -> detach
  -> release exporter state
```

v1에서 문제가 되는 부분은 attachment 또는 mapped PAL pointer가 다른 thread에
publish되는 시점과 revoke가 이를 회수하는 시점 사이에 명시적인 동기화가
충분하지 않다는 것이다.

## 8. 패치별 분석

### 8.1 01/26: DMA-BUF mapping types 도입

[패치 원문](https://lore.kernel.org/all/1-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

type, offer, match 구조와 exporter-first 중첩 매칭 알고리즘을 추가한다.
`dma_buf_mapping_type`, `dma_buf_mapping_match`, `dma_buf_match_args`, exporter ops
tag, `attach->map_type`이 핵심이다.

평가:

- 새 확장 지점을 만드는 핵심 패치다.
- pointer identity를 ABI처럼 사용하므로 객체 lifetime과 module ownership을
  명시해야 한다.
- `finish_match()`가 사실상 필수인데 NULL 허용 여부와 실패 계약이 불명확하다.
- forward declaration과 정의 중복 같은 정리할 부분이 리뷰에서 지적됐다.

### 8.2 02/26: SGT DMA mapping type 추가

[패치 원문](https://lore.kernel.org/all/2-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

기존 map/unmap 동작을 `dma_buf_sgt_exp_ops`로 옮길 기반과 importer/exporter/P2P
매크로 및 accessor를 추가한다.

평가:

- 기존 API와 새 generic API 사이의 migration anchor다.
- P2P distance 검사를 core에 모으는 방향은 타당하다.
- importer 요구와 exporter 능력을 하나의 enum에 함께 표현하여 역할이 섞인다.
- 잘못된 enum 값이나 flag 조합을 조용히 받아들이지 않도록 validation이 필요하다.

### 8.3 03/26: `dma_buf_mapping_attach()` 추가

[패치 원문](https://lore.kernel.org/all/3-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

generic attach와 exporter의 `.match_mapping`을 추가한다. 기존 API는 SGT 후보를
전달하는 wrapper가 된다.

중요 문제:

- 협상은 reservation lock 없이 실행된다.
- 협상 시점에는 `attach->dmabuf`를 포함한 일부 필드가 아직 초기화되지 않는다.
- 기존 dynamic attach API가 NULL `importer_ops`를 허용할 수 있는데 새 wrapper가
  `importer_ops->allow_peer2peer`를 바로 읽으면 NULL dereference가 발생한다.

v2는 callback이 읽을 수 있는 attachment 상태를 명시하고 NULL importer ops를
정규화하거나 명시적으로 거부해야 한다.

### 8.4 04/26: SGT 동작을 `attach->map_type`으로 라우팅

[패치 원문](https://lore.kernel.org/all/4-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

map/unmap/debugfs가 협상된 SGT ops를 사용하도록 바꾼다.

map과 unmap 경로가 attachment NULL 여부를 확인하기 전에 SGT accessor를 호출할
수 있는 순서 문제가 있다. public entry point의 validation을 먼저 수행한 뒤
type-specific accessor에 진입해야 한다.

### 8.5 05/26: 단일 offer exporter helper

[패치 원문](https://lore.kernel.org/all/5-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

하나의 mapping offer만 제공하는 exporter를 위한 `.single_exporter_match`와
`DMA_BUF_SIMPLE_SGT_EXP_MATCH`를 추가한다.

- `.match_mapping`과 상호 배타적이라는 계약이 필요하다.
- 파일 범위 compound literal의 static storage duration 사용 자체는 유효하다.
- helper가 boilerplate를 줄이지만 callback 조합 검증은 core에서 해야 한다.

### 8.6 06/26: DRM GEM transitional helper 보완

[패치 원문](https://lore.kernel.org/all/6-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

`drm_gem_map_dma_buf()`가 legacy callback과 새 SGT callback을 모두 인식하도록
한다. 단계적 전환을 위한 compatibility 패치이며 별도의 중대한 결함은 확인되지
않았다.

### 8.7 07/26: simple exporter 일괄 SGT 전환

[패치 원문](https://lore.kernel.org/all/7-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

amdxdna, ivpu, dma-heaps, udmabuf, armada, i915, msm, omap, tegra, virtio,
videobuf2, fastrpc, TEE, Xen, mbochs, fsl_asrc 등 다수 exporter를 기계적으로
변환한다. 직접 `attach->dev`를 읽던 경로도 SGT accessor를 사용한다.

이 패치는 변경 폭이 매우 크므로 다음 검증이 필요하다.

- 각 드라이버의 attach/map/unmap error path 보존
- P2P 허용 의미가 기존과 동일한지
- direction과 cache sync 동작 보존
- exporter별 CI 또는 최소 compile coverage

### 8.8 08/26: vmwgfx 전환

[패치 원문](https://lore.kernel.org/all/8-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

항상 실패하던 dummy attach/map/unmap을 mapping match의 `-EOPNOTSUPP`로
대체한다. 관찰 가능한 오류가 `-ENOSYS`에서 `-EOPNOTSUPP`로 달라질 수 있으므로
userspace 또는 테스트의 errno 의존성을 확인해야 한다.

### 8.9 09/26: habanalabs P2P-only 전환

[패치 원문](https://lore.kernel.org/all/9-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

MMIO 성격의 buffer를 P2P-only SGT exporter로 바꾸고 PCI distance 판단을 core로
옮긴다. 일반 SGT fallback은 제공하지 않는다. 장기적으로 physical vector를
SGT로 안전하게 변환하는 core helper의 사용 후보가 된다.

### 8.10 10/26: Xe 전환

[패치 원문](https://lore.kernel.org/all/10-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

MOVE_NOTIFY일 때 P2P를 우선하고 migratable BO에는 일반 SGT fallback을
제공한다.

중대한 bisectability 문제:

- `sgt_match[2]`에서 실제로는 `num_match`개만 초기화한다.
- matcher에는 `ARRAY_SIZE(sgt_match)`를 전달한다.
- 조건에 따라 초기화되지 않은 stack entry를 읽을 수 있다.
- 11/26에서 `num_match` 전달로 고쳐지더라도 10/26 단독 커밋은 안전해야 한다.

### 8.11 11/26: AMDGPU 전환

[패치 원문](https://lore.kernel.org/all/11-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

P2P 우선과 일반 fallback을 추가하고 GFX12 DCC에서 P2P를 제한한다. XGMI를 SGT
협상 위에 임시로 표현한다.

가장 명확한 correctness blocker는 다음 형태의 코드다.

```c
return attach->map_type.sgt_data.exporter_requires_p2p =
    DMA_SGT_EXPORTER_REQUIRES_P2P_DISTANCE;
```

대입되는 enum 값이 1이면 callback이 성공의 0이 아니라 1을 반환한다. 호출자가
이를 errno pointer로 바꾸면 `ERR_PTR(1)`처럼 `IS_ERR()`가 참으로 인식하지 않는
잘못된 포인터가 만들어질 수 있다. 의도는 대입 후 `return 0;`이어야 한다.

추가 문제:

- DCC 조건/주석 블록이 중복된 흔적이 있다.
- 이 패치가 Xe의 `ARRAY_SIZE` 문제를 고치지만 수정은 10/26에 포함돼야 한다.
- XGMI를 일반 P2P distance와 같은 모델에 넣는 것이 장기적으로 적절한지 별도
  검토가 필요하다.

### 8.12 12/26: VFIO PCI SGT 전환

[패치 원문](https://lore.kernel.org/all/12-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

VFIO PCI를 P2P-only SGT exporter로 바꾸고 revocation 및 vdev 검사를 match
callback으로 옮긴다.

Baolu Lu가 지적한 blocker:

- `dma_buf_mapping_attach()`는 reservation lock을 잡지 않는다.
- callback은 `dma_resv_assert_held()`를 호출하여 잘못된 locking 전제를 가진다.
- `priv->vdev` 접근이 cleanup과 경쟁하여 use-after-free가 될 수 있다.

후속 토론에서 제시된 수정 방향은 export 시 별도의 `struct pci_dev *pdev`
reference를 획득하고 release 시 put하며, match callback에서 live `vdev` 접근과
reservation-lock assertion을 제거하는 것이다.

관련 토론:
<https://lists.linaro.org/archives/list/linaro-mm-sig%40lists.linaro.org/thread/OCIJU7TEHU4HLVZ2NQM2PJKL5SMB7R44/>

### 8.13 13/26: physical vector-to-SGT helper 갱신

[패치 원문](https://lore.kernel.org/all/13-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

`dma_buf_phys_vec_to_sgt()` 관련 DMA IOVA map/unmap이 협상된 SGT DMA device를
사용하도록 바뀐다. 이 helper는 SGT attachment에만 유효하다는 전제를 API나
runtime assertion으로 분명히 해야 한다.

### 8.14 14/26: IIO buffer 전환

[패치 원문](https://lore.kernel.org/all/14-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

attachment device 비교를 SGT accessor 기반으로 바꾼다. non-SGT attachment에는
generic device가 없다는 새 모델을 소비자 코드에 반영한다.

### 8.15 15/26: FunctionFS 전환

[패치 원문](https://lore.kernel.org/all/15-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

FunctionFS도 직접 attachment device 접근을 SGT accessor로 바꾼다. 기존 device
동일성 검사와 오류 반환이 보존되는지 확인해야 한다.

### 8.16 16/26: legacy SGT 필드와 ops 제거

[패치 원문](https://lore.kernel.org/all/16-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

legacy `dma_buf_ops` map/unmap, attachment의 dev/peer2peer 필드와 compatibility
경로를 제거한다. 이후부터 mapping negotiation이 필수가 되므로 구조적인
point-of-no-return이다.

treewide 변환 누락을 잡기 위해 allmodconfig 계열 build와 out-of-tree 영향에
대한 명확한 migration note가 필요하다.

### 8.17 17/26: map API rename

[패치 원문](https://lore.kernel.org/all/17-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

`dma_buf_map_attachment{,_unlocked}`를 SGT 전용임이 드러나는
`dma_buf_sgt_*` 계열 이름으로 treewide 변경한다. semantic change보다는 API의
실제 범위를 이름에 반영하는 패치다.

### 8.18 18/26: unmap API rename

[패치 원문](https://lore.kernel.org/all/18-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

unmap API도 `dma_buf_sgt_*` 계열로 변경한다. map/unmap rename은 함께 적용될 때
리뷰와 bisect가 쉬운지 패치 분할을 다시 검토할 수 있다.

### 8.19 19/26: attach API rename

[패치 원문](https://lore.kernel.org/all/19-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

기존 SGT 전용 attach를 `dma_buf_sgt_attach()`로 명확히 이름 붙인다. generic
mapping attach와 legacy-compatible SGT attach의 구분이 API에 드러난다.

### 8.20 20/26: dynamic attach API rename

[패치 원문](https://lore.kernel.org/all/20-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

동적 구 API를 `dma_buf_sgt_dynamic_attach()`로 바꾸고 generic 경로는
`dma_buf_mapping_attach()`로 유지한다. 여기까지가 SGT migration의 논리적
완결점이다.

### 8.21 21/26: Physical Address List type 추가

[패치 원문](https://lore.kernel.org/all/21-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

`dma_buf_phys_list`와 PAL map/unmap ops를 추가한다. 각 entry는 physical/MMIO
range를 나타낸다.

문제:

- `length`가 entry count인지 byte length인지 naming만으로 불명확한 부분이 있다.
- alignment, nonzero length, 총 길이와 overlap invariant가 정의되지 않는다.
- map symbol은 `iommufd`로 제한하면서 unmap export 정책은 다를 수 있어
  비대칭이다.
- raw physical address API를 generic DMA-BUF 계층에 둘지 합의가 없다.

### 8.22 22/26: VFIO PCI PAL exporter

[패치 원문](https://lore.kernel.org/all/22-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

VFIO PCI private object가 physical vector를 보관하고 PAL을 첫 후보, P2P SGT를
fallback으로 제안한다. release 시 vector를 해제한다.

VFIO device teardown과 dma-buf release가 다른 순서로 진행되어도 vector와 PCI
device reference가 안전하게 유지되는지 증명해야 한다. refcount 관련 FIXME는
RFC 단계에서 정식 lifetime 모델로 바뀌어야 한다.

### 8.23 23/26: iommufd PAL importer

[패치 원문](https://lore.kernel.org/all/23-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

`symbol_get(vfio_pci_dma_buf_iommufd_map)` 형태의 순환 의존 hack과 전용 test
hook을 제거한다. iommufd importer는 PAL을 요청하며 초기 구현은 단일 range만
허용한다.

검토 사항:

- SGT fallback이 없어 PAL을 제공하지 않는 exporter는 사용할 수 없다.
- selftest map 경로가 `kvmalloc()`한 메모리를 이 단계에서 해제하지 않으면
  중간 커밋에 leak이 생긴다.
- attachment pointer와 mapped PAL pointer를 publish하는 시점이 revoke와
  직렬화되어야 한다.

### 8.24 24/26: iommufd multi-range 지원

[패치 원문](https://lore.kernel.org/all/24-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

reader가 현재 range index와 base를 저장하여 여러 PAL range를 PFN batch로
변환한다.

correctness와 보안 측면의 핵심 누락:

- `exp_phys->length`에 대한 index bounds check
- 빈 목록 처리
- 요청 크기보다 짧은 전체 PAL의 검출
- zero-length range에서 zero-progress loop 방지
- page alignment 검증
- `base + offset` 및 누적 길이 overflow 검증
- 요청 범위 전체가 매핑됐다는 종료 후 검증

신뢰할 수 없는 exporter 또는 단순 버그가 있는 exporter가 OOB read나 무한
loop를 일으키지 않도록 importer/core 경계에서 validation해야 한다.

### 8.25 25/26: multi-physical-range selftest

[패치 원문](https://lore.kernel.org/all/25-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

단일 range, 64-page range, 중간 slice, 분리된 subrange 및 domain 검증 테스트를
추가한다.

명확한 버그:

- map 측에서 `kvmalloc()`로 할당한다.
- unmap 측에서 `kfree()`로 해제한다.
- vmalloc fallback이면 잘못된 free이므로 `kvfree()`를 사용해야 한다.

추가로 malformed PAL, 빈 range, zero-length, 비정렬 주소, overflow, 정확한
boundary, 짧은 coverage, concurrent revoke 테스트가 필요하다.

### 8.26 26/26: mapping negotiation KUnit test

[패치 원문](https://lore.kernel.org/all/26-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)

약 374줄의 테스트로 no-match, callback 없음, `-EOPNOTSUPP`와 fatal error의
차이, exporter 우선순위, importer scan, 단일/복수 SGT exporter와 non-SGT
generic 경로를 검사한다.

누락된 범위:

- P2P distance와 topology 조건
- PAL invariant와 multi-range
- callback 중간 실패의 cleanup
- NULL importer ops
- revoke/detach concurrency
- module unload와 type object lifetime
- allocation failure injection

commit message의 `dma_bug_sgt` 표기는 오타로 보이며 정리해야 한다.

## 9. 병합을 막는 correctness 문제

| 우선순위 | 위치 | 문제 | 가능한 결과 | 필요한 조치 |
|---:|---|---|---|---|
| 1 | 11/26 AMDGPU | 성공 경로가 대입값 `1` 반환 | 잘못된 error pointer, crash | 대입 후 `return 0` |
| 2 | 12/26 VFIO | 존재하지 않는 resv lock 전제 | lockdep failure | assertion 제거와 계약 수정 |
| 3 | 12/26 VFIO | live `vdev` lifetime 경쟁 | use-after-free | 독립 `pci_dev` ref 보유 |
| 4 | 23~24/26 iommufd | attach/map publish와 revoke 경쟁 | UAF 또는 stale mapping | publish/revoke 직렬화 |
| 5 | 24/26 PAL walker | bounds 및 coverage 검증 부재 | OOB read, 무한 loop | 중앙 validation 추가 |
| 6 | 03/26 core | NULL `importer_ops` 역참조 | NULL dereference | NULL 정규화/검사 |
| 7 | 10/26 Xe | 미초기화 match entry 전달 | stack garbage 사용 | `num_match`만 전달 |
| 8 | 04/26 core | NULL 검사 전 accessor | NULL dereference | validation 순서 변경 |
| 9 | 25/26 selftest | `kvmalloc`/`kfree` 불일치 | invalid free | `kvfree` 사용 |
| 10 | 23/26 selftest | 중간 커밋의 unmap 누락 | memory leak | 같은 패치에서 cleanup |

이 표의 1~9는 최종 tree만이 아니라 해당 패치가 적용되는 각 중간 commit에서도
수정되어야 한다. Linux patch series는 각 commit이 buildable하고 합리적으로
testable해야 하므로 뒤 패치에서 우연히 문제가 사라지는 것으로 충분하지 않다.

## 10. 동시성과 lifetime 심층 분석

### 10.1 attachment 협상 시점의 lock

generic match callback은 reservation lock이 잡힌 상태에서 호출된다고 가정할 수
없다. exporter가 다음과 같은 객체에 접근한다면 별도의 reference나 lock을
가져야 한다.

- backing BO 또는 resource
- PCI device
- VFIO device private state
- revoke flag와 exported physical vector
- mapping type을 제공하는 module

callback API 문서에는 호출 context, sleep 가능 여부, 보유 lock, 재진입 가능성과
detach/release의 동시 실행 가능성을 기록해야 한다.

### 10.2 VFIO object graph

문제가 되는 object graph는 개념적으로 다음과 같다.

```text
dma_buf private state
   `-- priv->vdev ----> vfio device ----> pci_dev

cleanup thread:             attach thread:
  clear/free vdev             read priv->vdev
          \                    /
           `---- race --------'
```

dma-buf private state가 `vdev`보다 오래 살아도 raw pointer만 보유하면 안전하지
않다. attachment에 필요한 최소 객체가 `pci_dev`라면 export 시 그 reference를
직접 잡아 dma-buf release까지 유지하는 편이 lifetime 증명이 단순하다.

### 10.3 iommufd publish/revoke

다음 interleaving을 배제해야 한다.

```text
CPU A: attach succeeds
CPU B: revoke observes attachment and starts teardown
CPU A: mapped PAL pointer stores/publishes
CPU B: unmap/detach frees PAL backing
CPU A: stale pointer consumes
```

단순 NULL check만으로는 충분하지 않다. 하나의 mutex 아래에서 attachment와
mapped pointer를 publish/revoke하거나, refcounted state machine과 acquire/release
ordering을 정의해야 한다.

권장 상태 모델의 예시는 다음과 같다.

```text
NEW -> ATTACHED -> MAPPED -> REVOKING -> DEAD
          \          |
           `---------+----> detach only after users drain
```

각 전이의 소유 lock과 실패 rollback을 테스트해야 한다.

## 11. PAL 유지보수자 논쟁

### 11.1 반대 논리

Christian König은 raw PAL importer API에 강하게 반대했다. 과거 importer가
exporter의 backing physical address를 직접 취급하면서 주소 공간의 의미를
잘못 판단해 데이터 손상과 보안 문제가 반복됐다는 것이 핵심 이유다.

- 최초 반대:
  <https://www.mail-archive.com/dri-devel%40lists.freedesktop.org/msg600322.html>
- 사용 사례에 대한 Jason의 설명:
  <https://www.mail-archive.com/dri-devel%40lists.freedesktop.org/msg600403.html>

이 관점에서는 iommufd로 symbol을 제한하는 것만으로 충분하지 않다. 문제는
호출자 숫자가 아니라 physical address를 해석할 책임이 exporter 밖으로
나간다는 API 구조 자체이기 때문이다.

### 11.2 exporter-driven IOMMU mapping 대안

제안된 대안은 importer가 다음을 exporter에 제공하는 형태다.

- `iommu_domain`
- IOVA
- offset과 size
- 필요한 mapping attributes

exporter가 자신의 backing을 알고 있으므로 domain mapping을 직접 수행하고 raw
physical address는 importer에 노출하지 않는다.

관련 제안:
<https://www.spinics.net/lists/dri-devel/msg551956.html>

장점:

- exporter가 주소 종류와 이동 가능성을 통제한다.
- raw backing 주소가 API 경계를 넘지 않는다.
- cache/coherency 속성을 exporter가 반영할 수 있다.

한계:

- importer가 전달할 수 있는 독립 `iommu_domain`이 있어야 한다.
- KVM처럼 GPA 관리와 mapping 책임이 다른 방식으로 결합된 소비자에는 맞지
  않을 수 있다.
- rollback과 partial mapping API가 더 복잡해진다.

### 11.3 core converter 절충안

Jason이 논의한 절충안은 PAL을 exporter-facing 내부 표현으로 남기되 일반
importer가 직접 받지 않게 하는 것이다.

```text
exporter PAL offer
       |
       v
DMA-BUF core converter
       |
       +--> importer-requested iommu_domain mapping
       `--> validated SGT conversion
```

- exporter는 PAL을 offer할 수 있다.
- importer는 PAL 대신 IOMMU-domain type을 요청한다.
- core converter만 PAL과 domain 양쪽을 보고 mapping한다.
- 필요하면 PAL-to-SGT converter도 core 내부에 둔다.

관련 논의:
<https://www.mail-archive.com/dri-devel%40lists.freedesktop.org/msg602022.html>

이 구조는 raw address를 임의 importer에 공개하지 않는다는 장점이 있지만,
KVM 사용 사례에는 제공할 적절한 domain이 없다는 반론이 있다. 따라서 모든
소비자를 하나의 type으로 통합하려 하기보다 사용 사례별 신뢰 모델을 먼저
정리해야 한다.

## 12. API와 구현 개선 제안

### 12.1 협상 core

- offer와 match의 역할별 타입을 분리한다.
- 모든 enum과 flag 조합을 중앙에서 검증한다.
- `finish_match()` 계약과 rollback을 문서화한다.
- exporter ops와 mapping type object에 module reference를 연결한다.
- callback 호출 context와 locking을 kernel-doc으로 명시한다.
- NULL importer ops를 허용할지 금지할지 API 전체에서 통일한다.
- match callback이 attachment의 미완성 필드를 읽지 못하도록 인자를 최소화한다.

### 12.2 SGT 전환

- 10/26 Xe 수정은 해당 패치에 포함한다.
- 11/26 AMDGPU의 반환값과 중복 블록을 고친다.
- P2P distance, exporter requirement, importer permission을 별도 필드로 나눈다.
- legacy 제거 전에 allmodconfig와 주요 아키텍처 빌드 결과를 제공한다.
- 기존 errno 변화가 의도된 것인지 commit message에 기록한다.

### 12.3 PAL 또는 대체 모델

- range 구조에 `nr_ranges`와 byte length를 구분되는 이름으로 둔다.
- size, alignment, overflow, overlap, coverage를 map 전에 한 번 검증한다.
- MMIO/system RAM 및 cache 속성을 표현하지 못한다면 public generic API로
  노출하지 않는다.
- PAL access는 core converter 또는 명시적으로 신뢰된 importer에 한정한다.
- revoke state machine과 reference model을 먼저 정의한 후 코드를 작성한다.
- partial mapping 실패 시 이미 설치된 IOMMU entry rollback을 규정한다.

## 13. 권장 v2 패치 구성

리뷰 가능성과 병합 가능성을 높이려면 다음처럼 분할하는 것이 현실적이다.

### 단계 A: generic negotiation 기반

1. mapping type core와 문서
2. SGT type과 compatibility wrapper
3. negotiation KUnit tests
4. simple exporter 변환 helper

이 단계에서는 PAL을 포함하지 않고 API 자체의 locking, lifetime과 오류 계약을
합의한다.

### 단계 B: treewide SGT 전환

1. simple exporter batch
2. vmwgfx/habanalabs
3. Xe
4. AMDGPU
5. VFIO SGT
6. consumers와 API rename
7. legacy 제거

드라이버별 패치는 각각 독립적으로 build/test 가능해야 하며 Xe와 AMDGPU의
확인된 버그를 처음부터 수정해야 한다.

### 단계 C: non-SGT mapping 별도 RFC

1. 신뢰 모델과 사용 사례 문서
2. IOMMU-domain mapping type 또는 core converter
3. VFIO exporter lifetime 재설계
4. iommufd importer와 revoke state machine
5. malformed range 및 concurrency selftests
6. KVM처럼 domain 모델과 맞지 않는 소비자를 위한 별도 설계

PAL을 협상 core와 같은 series에 다시 묶으면 기반 API에 대한 긍정적 리뷰가 PAL
정책 논쟁에 가려질 가능성이 크다.

## 14. 테스트 계획

### 14.1 negotiation unit tests

- exporter 1개/여러 개 offer의 우선순위
- importer 1개/여러 개 match의 순회
- type pointer mismatch
- `-EOPNOTSUPP` fallback
- `-ENOMEM`, `-EINVAL` 등 fatal error 전파
- `finish_match()` 성공/실패와 cleanup
- NULL callback 및 NULL importer ops
- module unload 중 type lifetime

### 14.2 SGT/P2P tests

- P2P 허용/거부 importer
- 같은 PCI hierarchy와 불가능한 distance
- P2P-only exporter의 일반 SGT 거부
- migratable/non-migratable BO
- AMDGPU DCC 및 XGMI 조건
- Xe MOVE_NOTIFY 조합

### 14.3 PAL validation tests

- 빈 목록
- zero-length entry
- unaligned base/length
- address overflow
- range overlap
- 전체 길이가 dma-buf보다 짧거나 긴 경우
- 요청 offset이 range boundary에 있는 경우
- 여러 range를 가로지르는 partial request
- 마지막 entry 직전/직후 boundary
- 매우 큰 entry count와 allocation failure

### 14.4 concurrency tests

- attach와 revoke 경쟁
- map과 revoke 경쟁
- unmap과 dma-buf release 경쟁
- VFIO device removal 중 attach
- repeated map/unmap
- failure injection 후 reference leak 검사
- lockdep, KASAN, KCSAN, KFENCE 실행

## 15. 최종 평가

### 살릴 수 있는 부분

- DMA-BUF mapping 표현을 명시적으로 협상한다는 발상
- exporter가 실제 backing 제약에 따라 후보 우선순위를 통제하는 구조
- 선택된 결과를 attachment에 고정하는 `finish_match()` 모델
- 기존 SGT를 첫 type으로 전환하면서 API 의미를 명확히 하는 작업
- 협상 알고리즘을 대상으로 한 KUnit 기반

### v1에서 병합할 수 없는 부분

- AMDGPU의 잘못된 성공 반환
- VFIO의 잘못된 reservation-lock 전제와 object lifetime 경쟁
- iommufd PAL publish/revoke 경쟁
- PAL range walker의 안전성 검증 부재
- raw PAL importer API에 대한 유지보수자 합의 부재
- 중간 패치의 Xe uninitialized read와 selftest leak/free mismatch

### 결론

이 시리즈는 “DMA-BUF는 곧 SGT”라는 오래된 암묵적 계약을 깨고 주소 표현의
종류를 attachment 협상의 일부로 만들려는 중요한 시도다. 01/26~20/26의 방향은
API 계약과 변환 버그를 정리하면 독립적으로 발전시킬 가치가 충분하다.

그러나 PAL은 단순히 두 번째 mapping type을 추가하는 문제가 아니다. raw
physical address를 누가 볼 수 있는지, backing 이동과 revoke를 누가 통제하는지,
cache와 IOMMU mapping 책임이 어디에 있는지라는 별도의 보안 및 lifetime 모델을
요구한다. 이 모델이 합의되지 않은 상태에서 VFIO/iommufd private bridge를
generic PAL API로 치환하면 결합은 줄어들지만 위험한 책임은 API 전체로 퍼질 수
있다.

따라서 권장 방향은 협상 core와 SGT migration을 먼저 비-RFC 형태로 다듬고,
PAL/iommufd는 core converter 또는 exporter-driven IOMMU mapping을 중심으로
별도 RFC에서 다시 설계하는 것이다.

## 16. 참고 링크

- [Cover letter](https://lore.kernel.org/all/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/)
- [원문 thread view](https://lore.kernel.org/linux-media/0-v1-b5cab63049c0+191af-dmabuf_map_type_jgg@nvidia.com/T/#t)
- [개발 브랜치](https://github.com/jgunthorpe/linux/commits/dmabuf_map_type)
- [VFIO lifetime/locking 토론](https://lists.linaro.org/archives/list/linaro-mm-sig%40lists.linaro.org/thread/OCIJU7TEHU4HLVZ2NQM2PJKL5SMB7R44/)
- [PAL 반대 의견](https://www.mail-archive.com/dri-devel%40lists.freedesktop.org/msg600322.html)
- [PAL 사용 사례 설명](https://www.mail-archive.com/dri-devel%40lists.freedesktop.org/msg600403.html)
- [Exporter-driven IOMMU mapping 제안](https://www.spinics.net/lists/dri-devel/msg551956.html)
- [Core converter 절충안](https://www.mail-archive.com/dri-devel%40lists.freedesktop.org/msg602022.html)
