# JDoc 아카이브 확장 로드맵 (P2: 7z, P3: alz/egg) — reference 커버리지 추월

> 상태: **P2(7z)·P3(alz/egg) 구현 완료, reference 대비 벤치·실물 검증 완료.**
> P1(zip/gz/tar/tar.gz)은 JDLAB-1510 참조.
>
> **P4 (2026-07-20, 병렬 에이전트)**: 단독 BZ2/TAR.BZ2(`BzInflateStream`), RAR 4.x/5.x
> 헤더 워크+store 멤버(클린룸 MIT — unrar 배제 근거는 `docs/rar-feasibility.md`),
> HWP 3.x 텍스트 추출(`src/legacy/hwp3_parser`, 조합형 한글·표 셀 포함, reference·LibreOffice
> 교차검증). reference 대비 신규 우위: RAR5 store(reference 전체 거부), HWP3 표 셀(reference 파싱 중단).
> 잔여: RAR 압축 코덱(libarchive 기반 후속 플랜), 실물 hwp3 표 문서 검증.

## P5 플랜 — RAR 전체 디코딩 + 디코더 성능 (2·3번 구현 완료: 제로카피·멤버 병렬화)

1. **RAR 압축 코덱 (클린룸 MIT)** — libarchive(BSD-2) 리더 기반, 상세와 공수
   추정(RAR4 1–2주, RAR5 +1–2주)은 `docs/rar-feasibility.md` 후속 플랜 절 참조.
2. **제로카피 멤버 전달 (✅ 구현 완료, 2026-07-20)** — memcpy 벌크 복사(5번)와
   구분되는 별개 최적화: memcpy화는 "복사를 크게 묶어" 바이트 단위 루프 오버헤드를
   줄이는 것이고, 제로카피는 이미 메모리에 상주한 멤버 바이트를 **복사 없이
   포인터(뷰)로 파서에 전달**하는 것. 적용 지점:
   - 중첩 아카이브의 zip **store 멤버**: 부모 버퍼 안의 데이터 구간을 뷰로 직결
     (`DataSource::view_at`).
   - `MemoryStream` 기반 tar 멤버(중첩 tar): 스트림 뷰 직결(`InputStream::view`).
   - **7z solid 캐시 슬라이스(전 멤버, 압축 여부 무관)**: folder 디코드 버퍼의
     멤버 구간을 뷰로 직결 — 기존에는 folder 캐시 + 멤버 사본이 동시 상주(2×),
     제로카피로 1×. 피크 메모리·시간 모두 이득, 효과가 가장 큰 지점.
   - 캡 회계는 동일 유지(멤버·누적 캡을 뷰 크기로 선검사). deflate/bzip2/lzma
     압축 멤버는 디코딩 산출물이라 제로카피 대상이 아님.
   - 뷰의 수명 문제로 **단일 스레드(기본) 경로에서만 무복사**; 병렬 모드(3번)에서는
     워커에 넘길 때 사본 생성(병렬은 처리량 최적화, 제로카피는 기본 경로 최적화).
   - 후속 여지: alz/egg/rar store 멤버 뷰.
   - ❌ **최상위 파일 mmap 시도 후 폐기(2026-07-21)**: 이미지 추출 OFF 벤치에서는
     store −82%가 나왔으나, 그건 파서가 이미지 스트림을 안 읽어서 생긴 이득이었다.
     이미지 추출이 기본인 실사용에서는 시간 이득 0에 피크 RSS +37%라 되돌렸다.
     (`docs/decode-profile.md` 부록 F). `fseek(long)`→`pread` 오프셋 수정만 남겼다.
   - **구현**: `DataSource::view_at`/`InputStream::view`/`ZipReader::stored_view`/
     `TarReader::view_data`/`SevenZipReader::read_entry_view` + walker 뷰 경로
     (`precheck_view_member`로 캡 회계 동일 유지). 7z는 캐시 해제를 멤버 처리
     후로 이동(뷰 수명 보장).
   - **실측(2026-07-20, Apple Silicon 네이티브, Release)**: solid 7z(49MB 텍스트
     ×2, 1 folder)에서 피크 RSS **194.0MB → 144.9MB**(멤버 사본 49MB 제거 확증).
     시간은 동률(디코드 지배 워크로드라 기대대로). 단일 멤버 folder는 기존에도
     캐시 선해제라 피크 동일 — 이득은 멀티 멤버 folder에서 발생. 중첩 store
     멤버가 멤버 캡을 단독 초과하는 경우는 구조상 불가(부모가 같은 캡으로 먼저
     실체화)라 뷰 캡 검사는 심층 방어로만 동작.
3. **멤버 병렬화 (옵트인, ✅ 구현 완료, 2026-07-20)** — RAR 전용이 아니라
   zip/7z/alz/egg 전 포맷 공통 효과. 단일 스레드 원칙은 기본값으로 유지하고
   `ConvertOptions::archive`에 스레드 수 옵션(기본 1) 추가:
   - 전 포맷 공통 파이프라인: 디코드·순회는 호출 스레드 1개 유지 + **문서 변환만
     워커 N**에 위임(변환이 지배 비용). 결과 콜백은 호출 스레드에서 **순회 순서
     보존** 전달(순서 슬롯 큐).
   - 이 설계로 `WalkBudget`/`CapSink` 원자화가 불필요해짐 — 디코드·캡 회계가
     호출 스레드에 남으므로 스레드 안전 재설계 없이 성립(플랜 수정).
   - ~~zip/7z 멤버 파티션 병렬(디코드 자체 병렬)은 디코드가 전체의 10–20%라
     이득이 작아 후순위.~~ **정정(2026-07-21)**: 계측 결과 deflate zip에서 디코드는
     전체의 **77–83%**였다(`docs/decode-profile.md`). "10–20%"는 근거 없는 추정이었다.
     다만 이 경로는 연산이 아니라 메모리 대역폭에 막혀 있어, 디코더를 더 빠른
     구현으로 바꾸는 것만으로는 회수되지 않는다(libdeflate 실측 상한 2.6%, 부록 C). unrar식 코덱 내부 멀티스레드(rar5 디코드 분할)도 동일.
   - 인플라이트 멤버 수는 워커 수 기준으로 상한(메모리 = 상한 × 멤버 크기).
   - **구현**: `ArchiveLimits::threads`(1=단일 스레드 기본, 0=코어 수, N=워커 N),
     `src/archive/member_pipeline.{h,cpp}`(순서 슬롯 큐 + 백프레셔 2×워커,
     조기 중단 시 잔여 결과 폐기), CLI `--threads`, C API `archive_threads`,
     Python `threads` kwarg. 병렬 모드에서 뷰 멤버는 제출 시 사본 생성(수명).
   - **실측(2026-07-20, Apple Silicon 네이티브, M-코어 로컬)**:
     | 워크로드 | t=1 | t=4 | t=8 | 비고 |
     |---|---|---|---|---|
     | hwp 코퍼스 zip(store, 177문서 641MB) | 0.29s | **0.14s (2.1×)** | 0.15s | 아래 정정 참조 |
     | pdf+hwp zip(185문서 713MB) | 6.6s | **5.1s (1.3×)** | 5.2s | big.pdf 단독 4.9s가 Amdahl 하한(74%) — 하한 근접 |
     | reference 벤치 zip(40문서 27MB) | 0.096s | 0.080s | 0.078s | 소형이라 워크·디코드 오버헤드 지배 |
     스레드 1 vs 8 출력 완전 일치(zip/7z/alz/egg, out_bytes·순서·에러코드),
     테스트 76/76 그린(순서 보존·조기 중단·캡 강제 포함).
   - **정정(2026-07-21)**: 위 표의 "t≥4는 디코드 직렬 플로어"는 오진단이었다. 이
     워크로드는 store라 inflate가 아예 없다. 정본 머신(32코어) 재측정에서는 t=1부터
     평탄했고(0.486→0.444s), 이미지 추출 OFF 기준 실제 플로어는 멤버 버퍼의 익명
     페이지 폴트였다. 다만 이 플로어와 그 해법(버퍼 재사용)은 **이미지 추출을 끈
     경우에만** 유의미했고, 이미지가 기본인 실사용에서는 이득이 사라져 폐기했다
     (`docs/decode-profile.md` 부록 F).
4. ✅ 검증 완료(2026-07-20): reference 대비 재벤치를 **PII 검출 파이프라인 벤치**로 확장
   수행 — 결과·방법론은 `docs/pii-bench-report.md`(정본: Rocky 8.10 x86_64 네이티브
   dev-gpu-4, 교차: 맥 에뮬레이션 동일 매트릭스). 하네스·Dockerfile은 `test/bench/pii/`.
   핵심: 추출 zip 6.1×/7z 2.0×/alz 5.4×/egg 5.2× 우위(네이티브), reference는 소형 PDF
   텍스트 추출 손상으로 PII 누락(재귀 자체는 reference도 지원 — 진단 픽스처로 확인),
   RSS 전 구간 우위. 대용량 확장: 5중 재귀 258MB 13.0×, hwp 672MB 8.9×,
   **3.36GB 9~10×(검출 완전 동등)**. **XXL 테스트가 zip64 미지원을 노출 →
   ZipReader에 zip64 읽기 구현**(EOCD64+0x0001 extra, Entry 64비트화,
   test/fixtures/zip64/ 회귀 테스트).
   아래 2026-07-16 표는 변환 전용(구버전) 측정으로 유지.
5. **디코더 핫루프 최적화** (신규 RAR 엔진이 주 대상):
   - 64비트 프리페치 비트리더 리필(바이트 단위 리필 대체) — Huffman 디코드 1.5–2배.
   - LZ 매치 구간의 겹침 없는 범위를 memcpy 벌크 복사로 처리(복사 횟수·분기 감소;
     복사 자체를 없애는 2번 제로카피와는 별개).
   - 목표: 참조 구현(libarchive ~110MB/s, `-mt1` unrar ~410MB/s) 대비 200MB/s+.
   - 기존 코덱(deflate/bzip2/LZMA)은 라이브러리 디코더라 핫루프가 외부에 있음 —
     대신 `InputStream` 버퍼·`CodecSink` 호출 단위(64KB 배치)만 점검.

## reference 대비 벤치 결과 (2026-07-16)

환경: Docker Ubuntu 22.04 linux/amd64(Apple Silicon 에뮬레이션), 양쪽 동일 조건.
픽스처: docloom-corpus 40문서(hwp 24·pdf·doc·rtf·xls·docx·xlsx·ppt, 비압축 29.4MB)를
동일 내용으로 zip/7z/alz/egg 포장(`test/bench/make_bench_fixtures.py`).
측정: `/usr/bin/time -v` 3회 중앙값. JDoc=`bench_convert archive`, reference=`reference_img_test`
(이미지 추출 억제). 양쪽 모두 4개 포맷에서 멤버 40개 전부 처리(출력 바이트 자체 일관).

| 포맷 | JDoc 시간 | reference 시간 | 속도비 | JDoc RSS | reference RSS |
|------|----------|----------|-------|----------|----------|
| zip  | 0.60s | 2.47s | **4.1×** | 34.4MB | 48.7MB |
| 7z   | 2.76s | 3.74s | **1.4×** | 36.8MB | 42.6MB |
| alz  | 0.61s | 2.45s | **4.0×** | 33.4MB | 44.0MB |
| egg  | 0.72s | 2.63s | **3.7×** | 35.7MB | 40.9MB |

대형 멤버(고압축 텍스트 159.5MB 단일 멤버, alz/egg): JDoc 0.6s vs reference 17–19s(**~30×**),
피크 RSS는 양쪽 ~315MB로 동급(멤버+출력 사본 ≈ 2×멤버).

부수 발견 (실물 검증 겸):
- **실물 ALZip 생성 샘플**(EggDotNet 테스트 자산: defaults.alz, defaults_normal.egg 등
  8종)로 JDoc 검증 완료 — alz·egg·무압축 egg의 변환 출력이 서로 일치(69,990B),
  LZMA 멤버 포함 전부 정상. 암호화(AES)·AZO 멤버는 의도대로 멤버별 오류.
  → P3의 "실물 egg 검증 필요" 리스크 해소.
- **reference의 egg 지원은 부분적**: WinFileInfo(0x2C86950B) 레코드가 없으면 아카이브
  전체 거부, LZMA 멤버가 든 실물 egg(defaults_normal.egg)도 전체 거부(exit 20).
  JDoc은 둘 다 처리 — 커버리지에서도 우위.
- 우리 합성 alz는 reference가 그대로 수용(zip과 동일 출력) — 합성 픽스처의 실제품 호환 확인.

## 배경 / 목표

대조 솔루션 reference(사이냅 SN3 필터)는 zip/7z/rar/tar/gz/**alz/egg**/bzip 파서를 모두 갖췄다(디컴파일 소스 `~/Projects/reference-project/src/archive/`에서 확인). JDoc은 현재 zip/gz/tar만 지원하므로, 개인정보 검출 시나리오에서 국산 포맷(alz/egg)·7z 커버리지 갭이 존재한다.

목표: reference가 가진 포맷을 모두 지원하되, **성능·메모리·안전성에서 추월**한다.

### reference 대비 이미 확보한 우위 (P1에서 확인)
- **스트리밍 해제**: reference는 멤버를 `sn3malloc(uncompSize+1)`로 통째 할당(`sn3zip.cpp:445`) → 실측 멤버 크기의 4배 메모리. JDoc은 64KB 청크 스트리밍 → 멤버 1배. **P2/P3에서도 이 원칙 유지가 최우선 설계 목표.**
- **해제 중 캡 강제**: reference 제한 로직(`archiveOptionCheck`/`doesSkipArchiveFile`)은 옵션 존재하나 기본값 사실상 무제한(실측: 깊이 7+·600MB 멤버 통과). JDoc은 멤버 512MiB / 누적 64GiB / 깊이 3 기본 가드.
- **단일 API 표면**: C++/C/Python/CLI 일관.

### reference 참고 원칙 (IP + 코딩 스타일)
- 디컴파일 소스는 **포맷 구조·필드 레이아웃 이해용 참고**로 사용 (참고 허용됨).
- **명칭을 그대로 옮기지 말 것**: `reference_*`/`SN3*`/`t_SN3MFI`/`sn3zfs_*` 같은 이름·약어를 복사하지 말고, JDoc 스타일로 재명명 — 간결·명확한 함수/변수명, 기존 코드(`ZipReader`, `read_entry_streamed`, `DataSource`)와 일관된 네이밍·구조. 코드는 참고해 이해하되 **JDoc 코드베이스에 녹여 새로 작성**.
- 배포 코드는 **공개 명세 기반 클린룸** 원칙 유지 (상용 바이너리 디컴파일 코드 verbatim 복사 금지 — 구조 참고와 복사는 구분).
- GPL 코드(unalz 등) 참조 금지 — P3 alz에 특히 주의.

---

## Phase 2 — 7z ✅ 구현 완료 (2026-07-16)

구현 노트 (플랜과 달라진 점):
- SDK 버전 **25.01** (플랜 시점 목록에 없던 `Bcj2.{c,h}`가 `7zDec.c` 필수 의존이라 추가 벤더링 — BCJ2는 7z 기본 x86 필터라 디코더에 필요). `Precomp.h` 의존인 `7zWindows.h`, `RotateDefs.h`도 포함. PPMd는 기본 비활성(`Z7_PPMD_SUPPORT` 미정의) 유지 — 해당 멤버는 unsupported coder 오류.
- look-stream은 처음부터 새로 쓰지 않고 SDK의 `CLookToRead2`를 커스텀 `ISeekInStream`(read/seek → `DataSource::read_at` 위임) 위에 얹음 — Look/Skip 시맨틱 감사 리스크 제거.
- solid block 캐시: 같은 folder의 연속 멤버는 1회만 디코딩(`SzArEx_Extract` 캐시). oversized-solid pre-check는 플랜대로 디코딩 전 수행.
- 실측: 64MiB solid folder(4×16MiB) → 피크 RSS 98MiB(폴더+멤버 1개+오버헤드), 2.7s. reference 대비 Docker 벤치는 미실시(후속).
- 픽스처: `test/fixtures/7z/` 7종(각 <1KB, bigsolid만 712B), 테스트 9건 추가.

### 코덱: LZMA SDK 벤더링 (public domain)
- `vendor/lzma/`에 **디코더 전용** 파일만: `7zTypes.h 7zArcIn.c 7zAlloc.c 7zBuf.c 7zCrc.c 7zCrcOpt.c 7zDec.c 7zStream.c LzmaDec.{c,h} Lzma2Dec.{c,h} Bra.{c,h} Bra86.c Delta.{c,h} CpuArch.{c,h} Precomp.h Compiler.h`
- **제외**: `Threads.c`, `MtCoder.c`, `MtDec.c`, 모든 인코더 → **단일 스레드 보장** (CI grep으로 검증).
- 출처: 7-Zip 공식 LZMA SDK (7-zip.org/sdk.html), 라이선스 public domain — pugixml 벤더링과 동일 방침.
- CMake: `add_library(lzma_sdk STATIC ...)` (C 언어), `jdoc_archive`에 링크.

### 신규 파일
- `src/archive/seven_zip_reader.{h,cpp}`
  - `DataSource`를 SDK의 `ILookInStream`에 어댑팅 (커스텀 look-stream, seek/read를 `read_at`로 위임)
  - `SzArEx_Open` → 엔트리 목록, `SzArEx_Extract`로 folder(solid block) 단위 디코딩
  - API 형태는 `ZipReader`와 대칭: `entries()`, folder→멤버 매핑

### 워커 연동
- `src/archive/archive_walker.cpp`의 `walk_dispatch`에 `SEVENZIP` 브랜치 추가 (detect는 P1에서 이미 스테이징됨 — magic `37 7A BC AF 27 1C`, enum·확장자 준비 완료).
- **solid block 메모리 주의**: `SzArEx_Extract`는 folder 전체를 한 버퍼에 디코딩 → 피크 메모리 = solid block 크기. 완화책: 디코딩 **전에** folder 해제 크기를 `max_member_bytes`와 비교, 초과 시 그 folder의 멤버들을 `error="solid block exceeds size limit"`로 스킵.
- 개선 여지(선택): `Lzma2Dec` incremental API로 solid folder를 스트리밍 디코드 → reference 대비 추가 메모리 우위. 초기엔 pre-check로 충분, 후속 최적화로 분리.

### 테스트 (`test/test_archive.cpp` 확장)
- 일반 7z(LZMA), LZMA2 7z, solid 7z, 7z-in-zip 중첩, oversized-solid-folder 스킵.
- 픽스처: `p7zip`으로 생성해 소형 바이너리 체크인(zipfile처럼 코드 합성 불가).

### 리스크
- SDK 통합 자체가 이 페이즈 공수의 대부분(alloc 콜백, CRC init, look-stream 시맨틱 감사). 포맷 파싱보다 어댑터 검증에 시간 배분.
- `ILookInStream` 어댑터에서 seek 방향/오프셋 실수 주의.

---

## Phase 3 — alz / egg ✅ 구현 완료 (2026-07-16)

구현 노트 (플랜과 달라진 점):
- 포맷 근거는 `docs/alz-egg-format-notes.md`로 정리. 주 출처는 **EggDotNet(MIT)** 의
  레이아웃(클린룸 요건 충족) + reference 디컴파일의 시그니처/메서드 코드 교차확인.
  reference alz/egg 디컴파일은 대부분 스텁이라 참고 가치가 예상보다 낮았음.
- 두 리더 모두 tar처럼 **InputStream 순차 파싱**(DataSource 불필요). 공용 코덱 헬퍼
  `src/archive/codec_streams.{h,cpp}`(store/deflate/bzip2/lzma)를 신설해 두 리더가 공유.
  실패·캡 초과 시에도 멤버의 압축 바이트를 정확히 소진해 프레이밍을 보존 →
  멤버 단위 오류 후 순회 계속.
- ALZ/EGG 모두 헤더 CRC32를 스트리밍 중 검증(불일치 → "member crc mismatch").
- EGG 멀티블록 멤버 지원(블록별 독립 디코딩·CRC). split·전역 암호화는 아카이브 단위 오류.
- **후속 개선(2026-07-16, reference 디컴파일 정본화 후 재검토)**: 공식 명세 PDF(EggDotNet 동봉)
  기준으로 재정렬 — Extra Field 4바이트 size 플래그, 파일명 헤더 locale/parent-path-id
  길이 산술 수정(기존 EggDotNet 준거 구현은 locale 존재 시 2바이트 과독), 암호화 파일명
  멤버 격리, 전역 코멘트 허용, LZMA prop_size를 헤더 오프셋 2에서 파싱(reference 디코더 확인),
  ALZ 크기 필드 게이트를 descriptor 상위 니블(w!=0)로 수정, **solid EGG 스트리밍 demux
  지원**(멤버 1개 상주 유지). 실물 검증: EggDotNet 자산 34종 전수 — 의도된 오류(암호화
  10종·AZO 3종·분할 2종·전역암호화 1종) 외 전부 정상, solid_defaults.egg 출력이 non-solid
  변형과 69,990B 정확 일치, bzip2 ON 빌드에서 zeros.egg(197MB)·bz_optimized 8/8 통과.
- 검증: 합성 alz 픽스처를 **The Unarchiver(unar)** 로 추출 교차검증 — 멤버 내용 일치.
  (unar 꼬리 파싱 에러는 실제 ALZip이 종료 레코드 뒤에 쓰는 추가 구조 차이로 멤버와 무관.)
  EGG는 unar 미지원 포맷이라 교차검증 불가 — **실물 egg 샘플 확보 후 검증 필요**(잔여 리스크).
- 코퍼스(~/docloom-corpus)에 실물 alz/egg 없음 확인 — 실물 검증은 후속 작업.

### alz (`src/archive/alz_reader.{h,cpp}`)
- **클린룸**: 커뮤니티 문서 기반 (ALZip 포맷). reference `src/archive/sn3alz.cpp`는 필드 레이아웃 **참고만**, verbatim 복사 금지.
- 구조: `ALZ\x01` 시그니처 → per-file local header(`BLZ\x01`) → 메서드 store/deflate/bzip2.
- deflate는 기존 zlib raw inflate 재사용(`-MAX_WBITS`). 스트리밍 유지.
- 파일명: **CP949** → `util::cp949_to_utf8` (P1에서 이미 사용 중).
- 암호화 멤버: `error="encrypted member unsupported"`.

### egg (`src/archive/egg_reader.{h,cpp}`)
- ESTsoft **공식 공개 명세**(rev 1.3) 기반. reference `sn3egg.cpp` 참고 가능하나 명세 우선.
- 구조: `EGGA` magic → 블록 기반(파일 헤더/블록/암호화 헤더) → 메서드 store/deflate/bzip2/lzma.
- lzma 메서드는 **P2에서 벤더링한 `LzmaDec` 재사용**.
- 파일명: **UTF-8**(egg는 UTF-8 저장).
- 미지원: AZO(ESTsoft 독점 코덱, 명세 없음), solid, 분할 볼륨 → 멤버별 명확한 에러.

### bzip2 (alz/egg 공통)
- CMake 옵션 `JDOC_WITH_BZIP2`(기본 OFF): 시스템 libbz2 링크.
- OFF일 때 bzip2 멤버는 `error="bzip2 member unsupported (build without JDOC_WITH_BZIP2)"`.
- 소수 메서드라 vendoring보다 optional dep가 합리적.

### 워커 연동
- `walk_dispatch`에 `ALZ`/`EGG` 브랜치 (detect는 P1 스테이징: magic `ALZ\x01`, `EGGA`).
- **관대한 실패**: 미지 블록 타입은 walk 중단이 아닌 skip+error로 처리.

### 테스트
- 실물 샘플로 검증(ALZip으로 생성한 alz, 알집/공식 도구로 생성한 egg). 버전별 편차 확인.
- store/deflate/bzip2/(egg)lzma 각 메서드, CP949 이름(alz), 암호화 에러, AZO 에러.

### 리스크
- alz 명세 신뢰도가 7z보다 낮음 → 실물 코퍼스로 검증, 미지 블록에 관대하게.
- egg AZO는 처음부터 미지원 명시.

---

## 페이즈별 실행 체크리스트 (새 세션용)

각 세션 시작 시:
1. 이 문서 + `docs/`의 P1 구현(`src/archive/`, `src/zip_reader.*`, `src/convert_internal.h`) 숙지.
2. `detect_format_mem`(src/jdoc.cpp)에 해당 포맷 magic·enum·확장자가 **이미 준비돼 있음** 확인 — 파서와 `walk_dispatch` 브랜치만 채우면 됨.
3. 스트리밍/캡 원칙 유지: 멤버를 통째 할당하지 말 것. `CapSink`(archive_walker.cpp) 패턴 재사용.
4. 구현 → `test_archive` 확장 → 기존 테스트 회귀 확인 → 벤치(`bench_convert`)로 reference 대비 재측정.
5. 완료 시 JDLAB-1510 하위 작업으로 결과 이슈 생성(jira-writing 스킬).

## 검증 (공통)
- `test_office`/`test_pdf`/`test_archive` 그린.
- 단일 스레드(기본 경로): `vendor/lzma/`에 `Threads.c` 미포함, `src/archive/`의
  스레드 사용은 옵트인 파이프라인(`member_pipeline.{h,cpp}`)에 한정 —
  `ArchiveLimits::threads` 기본값 1이면 스레드 생성 자체가 없음.
- 메모리: `/usr/bin/time -l`로 solid 7z·대형 alz 멤버가 캡 내 유지 확인.
- reference 대비: 동일 Docker(linux/amd64)에서 동일 픽스처로 시간·maxRSS 비교. 목표: **reference보다 빠르고 메모리 적게**.

## 참고 위치
- JDoc P1 구현: `src/archive/{data_source.h,input_stream.*,tar_reader.*,archive_walker.cpp}`, `src/zip_reader.*`, `include/jdoc/archive.h`, `include/jdoc/types.h`(ArchiveLimits)
- reference 디컴파일 참고: `~/Projects/reference-project/src/archive/{sn3sevenzip.cpp,sn3alz.cpp,sn3egg.cpp,sn3zfs.cpp}`, `include/sn3archive.h`
- 벤치: `test/bench_convert.cpp`, Docker `~/Projects/JDoc` 마운트 후 `bench_convert archive <fix>`
- 코퍼스: `~/docloom-corpus`
