# RAR 직접 파싱 — 타당성 판단과 구현 범위

> 상태: **RAR 4.x/5.x 헤더 워크 + store 멤버 추출 구현 완료** (`src/archive/rar_reader.{h,cpp}`).
> 압축 멤버(LZSS+Huffman+PPMd)는 라이선스 제약으로 미구현 — 멤버별 오류로 보고.

## 라이선스 판단

unrar 라이선스(rarlab 배포 소스 동봉, license.txt)의 요지:

- unrar 소스는 "RAR 아카이브를 다루는 어떤 소프트웨어에서든 무료 사용 가능"하나,
  **RAR 압축 알고리즘 재구현(아카이버 개발)에 사용 금지**라는 사용 제한이 붙는다.
- 수정 배포 시에도 "RAR 호환 아카이버 개발에 사용 불가" 문구를 유지해야 한다.
- 이 사용 제한 때문에 Fedora는 unrar 라이선스를 **비자유(BAD)로 분류**하고
  소스·바이너리 모두 배포에서 제외한다.

JDoc은 순수 MIT를 유지하므로 다음을 배제했다.

1. **(b) 공식 unrar 소스 벤더링 배제** — 압축 해제 용도 사용은 허가되지만, 코드가
   unrar 라이선스(비-OSI, 사용 제한)로 남아 MIT 트리에 제한 조항이 유입된다.
   기존 벤더링 선례(public domain LZMA SDK, MIT pugixml)와 달리 permissive가 아니다.
2. **(a) reference 디컴파일 소스 참고 배제** — `sn3rar_unpack.cpp`는 스스로
   "adapted unrar"라 명시한 unrar 행동 재구성물이다. 맹글링된 심볼에 unrar
   원본 클래스명(`CItemEx`, `CRarTime`, `CInArchive`, `NArchive` 네임스페이스)이
   그대로 남아 있어 파생 관계가 심볼 수준에서 확인된다. 이를 참고해 디코더를 쓰면
   라이선스 표기 없는 unrar 파생물이 되어 (b)보다 나쁘다. 헤더 레이아웃 같은
   순수 포맷 사실 확인만 허용했다.
3. **완전 클린룸 전체 디코더 보류** — RAR 2.9 해제는 LZSS+Huffman에 PPMd var.H
   블록이 동적으로 섞이고 VM 필터(delta/x86)까지 필요하다. 독립 명세가 없어
   수천 라인·수 주 공수이며, PPMd 없는 절반 구현은 실아카이브 다수가 멤버 중간에
   실패한다.

**채택 경로**: 헤더 레이아웃은 공개된 포맷 사실(RARLAB technote, libarchive(BSD)
`archive_read_support_format_rar{,5}.c`, python rarfile 문서)이므로, unrar 비파생
클린룸 MIT 구현으로 **헤더 워크 + store(무압축) 멤버 추출**만 지원한다.
RAR 압축 알고리즘 코드는 일절 포함하지 않는다.

## 구현 범위

| 항목 | 동작 |
|------|------|
| RAR 4.x (`Rar!\x1A\x07\x00`) | 헤더 워크, store(0x30) 추출, CRC32 검증, CP949·유니코드 인코딩 파일명 복원 |
| RAR 5.x (`Rar!\x1A\x07\x01\x00`) | 헤더 워크(vint, 헤더 CRC 검증), store 추출, UTF-8 파일명 |
| 압축 멤버 (0x31–0x35 / rar5 method 1–5) | `rar compressed member unsupported` 멤버별 오류, 순회 지속 |
| 암호화 멤버 | `encrypted member unsupported` 멤버별 오류 |
| 헤더 암호화(-hp) | `encrypted rar archive unsupported` 아카이브 오류 |
| 분할 볼륨 멤버 | `split rar member unsupported` 멤버별 오류 |

스트리밍·캡 원칙은 기존과 동일: 순차 `InputStream` 워크, 멤버 통째 신뢰 없이
`CapSink`가 해제 중 바이트를 계수(walker 공통 경로).

## 검증 (2026-07-20)

- 픽스처: RAR5는 실제 rar CLI 7.23 생성(`test/fixtures/rar/`, store·압축·암호화·
  헤더 암호화 4종). RAR4는 rar 7.x가 4.x 생성을 지원하지 않아 테스트에서 코드 합성
  — 합성 산출물은 공식 unrar `t`(All OK), lsar, unar 교차 검증 통과.
- `test_archive` 65개 전부 통과(RAR 13개 신규: store·CP949·유니코드 이름·압축/암호화/
  CRC 불일치/절단 오류 경로·zip 중첩·매직 감지).
- reference 대조(Docker linux/amd64): 합성 RAR4 store를 reference도 동일 내용으로 추출(호환
  확인). **RAR5는 reference가 전체 거부(exit 170)** — RAR5 store 커버리지는 JDoc 우위.
  단, RAR4 압축 멤버는 reference(unrar 파생 엔진 내장)가 해제하는 반면 JDoc은 오류 보고.
- **실제품 RAR4 검증 완료**: libarchive 테스트 스위트(BSD-2)의 실제 WinRAR 산출
  RAR4 3종으로 확인 — store 멤버 내용 일치, 일본어 유니코드 파일명 lsar와 동일
  복원, 압축 멤버·분할 볼륨(part0001)은 의도된 멤버별 오류. 소형 2종은
  `test/fixtures/rar/rar4_libarchive*.rar`로 체크인(출처 명기).

## 후속 플랜 — 전체 디코딩 (미착수)

libarchive(BSD-2, MIT 호환)의 RAR 리더를 근거 자료로 한 클린 경로가 존재한다:

- RAR4: `archive_read_support_format_rar.c`(~3.3k 라인) — Huffman+LZSS,
  PPMd var.H은 public domain Ppmd7(`archive_ppmd7.c`) 기반. 참고 구현 또는
  부분 벤더링(BSD 고지 유지) 모두 가능.
- RAR5: `archive_read_support_format_rar5.c`(~4k 라인) + RARVM 없는 필터 처리.
- 예상 공수: RAR4 해제 엔진 이식·검증 1–2주, RAR5 포함 시 +1–2주.
  입력 스트림 어댑터(libarchive read API → `InputStream`)와 solid 아카이브
  메모리 상한 설계가 주된 작업.
- 성능 실측(2026-07-20, RAR5 `-m3` 91MB→14MB, 동일 출력 확인, 단일 스레드 보정):
  unrar 기본값은 멀티스레드(CPU>wall)라 공정 비교는 `-mt1` 기준 —
  **unrar ~410MB/s vs libarchive ~110MB/s, 실격차 약 3.6배**(구현 성숙도 차이:
  수제 비트리더·64비트 창 복사 vs 범용 루프). 엔드투엔드 환산(29.4MB 코퍼스)으로는
  해제 0.27s vs 0.07s — 전체 시간의 10–20% 수준이라 실사용 병목 아님, reference 대비
  총합 우위 유지. 이식 시 최적화 여지: 64비트 프리페치 비트리더 리필 + 매치 구간
  memcpy화로 200MB/s대(공식 대비 ~2배 이내) 도달 가능 추정 — 논문 정량 꼭지 후보.

남은 리스크: ~~실제품 RAR4 픽스처 부재~~ → libarchive 테스트 자산으로 해소(위 검증
절). 잔여는 솔리드·대용량 볼륨 조합 정도로, 전체 디코딩 착수 시 함께 다룬다.
