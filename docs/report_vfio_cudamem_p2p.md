# VFIO 격리 하의 NVMe ↔ CUDA GPU 직접 P2P DMA 조사 보고서 (v2)

## 0. 요약 (Executive Summary)

유저스페이스에서 NVMe를 VFIO로 제어하면서 CUDA GPU 메모리를 NVMe의 직접 P2P DMA
버퍼로 쓸 수 있는지 조사했다. 기존 보고서(7장)는 "도메인 불일치로 불가"라
결론냈으나, 본 조사에서 **커널 모듈이 GPU 물리주소를 NVMe가 사용하는 VFIO IOMMU
도메인에 직접 `iommu_map` 하는 경로(옵션 A)** 를 구현하고 통제 실험을 수행하여 그
결론을 정정한다. 핵심 결과는 **방향 비대칭**이다.

- **스토리지 → GPU (데이터 로딩, NVMe READ 명령): VFIO 격리를 유지한 채 직접
  P2P로 동작하며, 데이터 무결성(Read–Write–Read 라이브니스)까지 확인되었다.**
- **GPU → 스토리지 (저장, NVMe WRITE 명령): 실패한다 (NVMe Data Transfer Error).**

즉 ML 데이터 로딩·가중치 적재 등 "스토리지→GPU 읽기"는 이 소비자 플랫폼에서도
격리하에 동작한다. 실패하는 것은 반대 방향(GPU→스토리지)뿐이며, 일련의 통제 실험
결과 그 원인은 소프트웨어/도메인/ATS가 아니라 **PCIe non-posted read의 P2P
포워딩 한계**로 좁혀졌다.

## 1. 시험 환경

| 항목 | 값 |
|---|---|
| CPU / IOMMU | Intel Core i9-14900K, VT-d (`intel_iommu=on`, passthrough 아님) |
| RAM | 64 GiB |
| 커널 | 6.8.12-dmabuf (custom) |
| GPU | NVIDIA Open KMD 580.159.03, BAR1 = `0x6000000000`(32 GiB, prefetchable) |
| NVMe | Samsung 980 PRO @ `0000:02:00.0`, `vfio-pci` |
| 토폴로지 | NVMe = root port `00:01.1`, GPU = root port `00:01.0` (서로 다른 root port) |
| ACS (root port) | `001d` = SrcValid+ ReqRedir+ CmpltRedir+ UpstreamFwd+, DirectTrans- |
| ATS | NVMe·GPU **모두 미지원** |

## 2. 배경과 문제

`cudamem` 힙은 GPU 메모리를 dma-buf로 export 하고 udmabuf 경로로 페이지별
물리주소표(`phys_lut`, 값은 GPU BAR1 주소)를 만든다. UIO에서는 IOMMU 번역이 없어
`phys_lut`를 PRP에 그대로 넣어 P2P가 됐다. 그러나 VFIO에서는 IOMMU가 NVMe DMA를
번역하므로, PRP의 IOVA가 NVMe가 쓰는 도메인에 매핑되어 있어야 한다.
`VFIO_IOMMU_MAP_DMA`/`IOMMU_IOAS_MAP`은 **핀 가능한 host VA**만 받아 CUDA device
VA를 등록할 수 없고(`-EFAULT`), dma-buf mmap도 `-ENOTSUPP`였다.

## 3. 구현: 옵션 A — 커널 모듈이 직접 `iommu_map`

통찰은 "번역할 주소(`phys_lut`)를 이미 안다"는 것. 빠진 것은 그 물리주소를 **NVMe가
실제로 사용하는 VFIO 도메인**에 넣는 행위였다. `upcie_dmabuf_importer.ko`에 신규
ioctl `UPCIE_DMABUF_IOMMU_MAP`을 추가했다.

1. 유저가 `phys_lut` + 사용자 선택 `iova_base`를 전달.
2. 모듈이 `iommu_get_domain_for_dev(&nvme->dev)`로 NVMe의 현재 도메인(VFIO 소유,
   type=1 UNMANAGED)을 얻음.
3. `iommu_map(domain, iova_base+i·ps, phys_lut[i], ps, READ|WRITE|MMIO)`.
4. 유저는 PRP에 IOVA를 넣는다.

`iommu_iova_to_phys`로 역검증하여 페이지테이블이 정확함을 확인했다
(`0x4000000000 → 0x602f600000`, 기대값 일치).

## 4. 통제 실험 (단일 변수 = 번역 출력 타깃)

`iova_base = 256 GiB` 고정, 매 경우 `iommu_map` 성공·DMAR fault 없음.

| # | 동작 | 종류 | 타깃 | 결과 |
|---|---|---|---|---|
| A | NVMe가 호스트 RAM을 읽음 (WRITE) | non-posted read | 시스템 RAM | **성공** |
| B | NVMe가 GPU BAR를 읽음 (WRITE) | non-posted read | peer BAR | **실패** (SC=0x4) |
| C | NVMe가 GPU BAR에 씀 (READ) | posted write | peer BAR | **성공** |

**Read–Write–Read 라이브니스 검증**: 디스크에 패턴 A 기록→디스크→GPU 읽기(GPU==A,
0/82 mismatch)→디스크를 B로 변경→다시 읽기(GPU==B, 0/82 mismatch). GPU 버퍼를 매번
sentinel(0x5A/0xA5)로 선채움 후 P2P write가 라이브 디스크 값으로 바꾸는지 확인하여,
**스토리지→GPU 경로가 올바른 라이브 데이터를 전달**함을 입증했다.

세 실험의 단일변수 비교 결론:
- A vs C: **소프트웨어(iommu_map·IOVA·PRP·번역) 완전 정상**.
- A vs B: **non-posted read 자체나 高IOVA가 문제 아님** (RAM 대상이면 read 성공).
- 유일 실패 조합 = **"non-posted read + 타깃이 peer GPU BAR"**.

PCIe상 쓰기는 posted(편도, completion 불필요), 읽기는 non-posted(왕복, completion
필요)다. C(쓰기)는 completion이 없어 성공하고, B(읽기)는 GPU가 만든 completion이
다른 root port의 NVMe로 되돌아오는 왕복이 필요해 실패한다.

## 5. 근본 원인 추적 (배제법)

P2P read 실패 지점을 (A) RC가 non-posted P2P 요청 미포워딩, (B) ACS CmpltRedir가
completion 회송 차단, (C) GPU가 peer read 거부 — 세 후보로 두고 검증했다.

- **드라이버 버그 / bus master 배제**: 동일 WRITE 명령 코드가 호스트 버퍼로는
  성공([W1]/[W2]). 유일 차이는 버퍼 위치(호스트 RAM vs GPU BAR)뿐. bus master가
  꺼졌다면 IDENTIFY·호스트 DMA부터 전부 실패했을 것이나 모두 성공.
- **NVMe Error Information Log(LID 0x01)**: 실패 직후에도 **전부 0**(저널 없음).
  → 미디어/명령 에러가 아니라 per-command으로만 보고되는 전송 레벨 실패.
- **ACS 토글(setpci, 양 root port `001d`→`0000`→복원)**: ACS 완전 비활성에도 P2P
  read **여전히 실패**. → **후보 B(ACS CmpltRedir) 배제**.
- **경로 4개 디바이스 PCIe 에러 캡처(실패 직후)**: NVMe·GPU·양 root port 모두
  `DevSta: CorrErr- NonFatalErr- FatalErr- UnsupReq-`, AER `UESta/CESta` clean,
  커널 AER 카운터 전부 0, dmesg DMAR/AER 없음, nvidia-smi Xid/ECC 무에러.
  → **어디에도 PCIe 에러 흔적이 없음.**

**해석**: GPU가 read를 거부(C)했다면 어딘가 `UnsupReq+`/`CmpltAbrt+`/Xid가 찍혀야
하나 전무하다. completion timeout이면 `CmpltTO+`가 있어야 하나 없다. "write는 되고
read만 실패" + "모든 에러 레지스터 침묵"을 동시에 설명하는 유일한 가설은 **(A) root
complex가 서로 다른 root port 간 non-posted(read) P2P 요청을 조용히 포워딩하지
않음**이다. 요청이 GPU에 도달조차 못 해 completion이 생성되지 않고, NVMe는 읽기
데이터를 못 받아 내부적으로 전송을 실패 처리(SC=0x4)하되 PCIe 에러는 발생하지
않는다. 이는 Intel 클라이언트 root complex가 root port 간 P2P에서 posted write는
지원하나 non-posted read는 지원하지 않는, 알려진 동작과 일치한다.

확정의 마지막 한 조각은 **UIO(IOMMU 번역 제거) 양방향 테스트**다. UIO에서도 read가
실패하면 (A) RC 근본 비포워딩으로 확정, 성공하면 "VT-d 번역 상호작용"으로 정정된다.
소프트웨어 레지스터로는 "요청이 GPU에 도달했는지"를 직접 못 보므로, 그 이상은 PCIe
버스 애널라이저(TLP 단위 관측)가 필요하다.

## 6. 기존 결론(7장) 정정

기존 보고서는 실패를 "helper LUT가 VFIO 도메인에 없어서(도메인 불일치)"로
단정했으나 이는 검증 없는 추정이었다. 옵션 A가 **올바른 물리주소를 VFIO 도메인에
정확히 매핑·번역(역검증 완료)** 하고도 read만 실패함을 보였으므로, 원인은 "도메인
불일치"가 아니라 **"플랫폼이 non-posted P2P read(peer BAR 대상)를 완료하지 못함"**
으로 정정한다.

## 7. 결론 및 권고

- **결론**: 이 소비자 Intel 플랫폼에서 **VFIO 격리 + 스토리지→GPU 직접 P2P 로딩은
  동작**한다(무결성 검증 완료). **GPU→스토리지(P2P read)만 불가**하며, 그 원인은
  소프트웨어/ATS/도메인이 아니라 **RC의 non-posted P2P read 비포워딩**으로 좁혀졌다.
- **로딩 중심 워크로드**(추론/학습 데이터 적재, 가중치 로드): 옵션 A로 **지금
  격리를 유지하며 사용 가능**.
- **양방향이 필요하면**: ① UIO/passthrough(격리 포기, 단 read 가능 여부는 별도
  확인 필요), ② **NVMe·GPU를 같은 PCIe 스위치 아래** 배치(P2P가 스위치 내부 처리 →
  read 가능성 ↑, 보통 서버보드), ③ ATS 지원 데이터센터 디바이스. **CPU만 AMD
  EPYC로 교체하는 것은 단독으로는 불충분**하며, 핵심은 "RC/토폴로지가 non-posted
  P2P read를 처리하는가"이다.
- **다음 단계**: (1) UIO 양방향 테스트로 RC 비포워딩 vs 번역 상호작용 확정,
  (2) 가용 시 EPYC/공통-스위치 토폴로지에서 재현.

## 부록: 산출물

- 커널 모듈: `kernel/upcie_dmabuf_importer.c` (`UPCIE_DMABUF_IOMMU_MAP`,
  `iommu_iova_to_phys` 역검증, 진단 로그)
- UAPI/래퍼: `include/upcie/dmabuf_importer.h`
- 테스트: `tests/test_cudamem_iommu_map_nvme_readwrite.c` (R-W-R + Error Log 덤프),
  `tests/test_hostmem_iommu_map_nvme_readwrite.c` (호스트 RAM 대조)
- 진단 절차: ACS 토글(`setpci ECAP_ACS+0x6.w`), 4-디바이스 DevSta/AER 캡처,
  NVMe Get Log Page(LID 0x01)
