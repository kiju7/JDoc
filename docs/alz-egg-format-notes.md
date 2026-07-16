# ALZ / EGG 포맷 정리 (P3 구현 근거)

구현 근거 문서. 출처 우선순위: **ESTsoft EGG Format Specification v1.0 공식 문서**
(EggDotNet 저장소 동봉 PDF) > EggDotNet(MIT) 레이아웃 > reference 디컴파일(시그니처·메서드
코드·LZMA 페이로드 형식 교차확인). GPL 구현(unalz 등)은 참조하지 않음.
모든 정수는 little-endian.

## ALZ

```
[전역 헤더]
  u32 magic       0x015A4C41  "ALZ\x01"
  u16 version     (ALZip 생성본은 10)
  u16 header_id

[로컬 파일 헤더]*  — magic으로 식별하며 순차 나열
  u32 magic       0x015A4C42  "BLZ\x01"
  u16 name_len
  u8  attribute
  u32 time        (DOS time)
  u8  descriptor  — bit0: 암호화(데이터 앞 12바이트 암호 헤더 추가), bit3: extra
                    상위 니블 = 크기 필드 폭 w ∈ {1,2,4,8}
  u8  reserved
  [w != 0 인 경우만 — descriptor가 플래그 비트만 갖는 경우(w=0) 이 필드들 없음]
    u8  method    0=store, 1=bzip2, 2=deflate(raw)
    u8  bit_flag
    u32 crc32     (비압축 데이터의 CRC32)
    uW  compressed_size
    uW  uncompressed_size
  u8[name_len] name   (CP949 또는 UTF-8; UTF-8 유효성 검사 후 아니면 CP949 변환)
  [암호화 시 u8[12] 암호 헤더]
  u8[compressed_size] data

[종료]
  u32 magic       0x025A4C43  "CLZ\x02"
  (0x015A4C43 "CLZ\x01" = central-directory류 구조가 올 수 있음 — 만나면 순회 종료)
```

bit_flags == 0이면 크기/메서드 필드가 아예 없음(디렉토리 엔트리). 디렉토리는 attribute
bit4(0x10)로도 표시되지만 크기 0으로 걸러도 충분.

## EGG

레코드 스트림 구조. 모든 레코드는 4바이트 magic으로 시작하고, "확장 필드"류는
공통적으로 `u8 bit_flag + size + u8[size] data` 형태(안전한 skip 가능).
**size는 bit_flag bit0가 0이면 u16, 1이면 u32** (공식 명세 Extra Field 정의).
size는 magic/bit_flag/size 자신을 제외한 데이터 길이.

```
[EGG 헤더]
  u32 magic       0x41474745  "EGGA"
  u16 version     0x0100
  u32 header_id
  u32 reserved
  [확장 필드]*    — magic 나열, 아래 둘이 오면 해당 아카이브는 미지원 처리
    0x24F5A262 split (분할 볼륨)
    0x24E5A060 solid
  u32 0x08E28222  EOFARC — 헤더 종료

[파일 단위]*
  u32 0x0A8590E3  file header magic
    u32 file_id
    u64 file_length          (파일 전체 비압축 크기)
  [확장 필드]* — 파일 헤더 구간:
    0x0A8591AC filename: u8 flags + size(2|4) + [flags&0x08: u16 locale]
                         + [flags&0x10: u32 parent_path_id] + name
                         name 길이 = size − locale(2) − parent_id(4)
                         flags&0x04 = 파일명 암호화 → 멤버 단위 오류로 격리
                         locale 949 또는 UTF-8 비유효 시 CP949 → UTF-8 변환
    0x2C86950B win file info   (본문 9바이트: mtime 8 + attr 1)
    0x1EE922E5 posix file info
    0x08D1470F encrypt header  → 해당 멤버 암호화(미지원 오류)
    0x04C63672 comment / 0x07463307 dummy / 0xFFFF0000 skip
  u32 0x08E28222  EOFARC — 파일 헤더 구간 종료
  [블록]*         — file_length를 채울 때까지 반복 (대부분 1블록; 4GB 초과 파일은 다중 블록)
    u32 0x02B50C13 block header magic
    u8  method     0=store, 1=deflate(raw), 2=bzip2, 3=AZO(독점, 미지원), 4=lzma
    u8  hint
    u32 uncompressed_size
    u32 compressed_size
    u32 crc32
    [Extra Field 3]* → u32 0x08E28222 EOFARC로 종료 (보통 즉시 EOFARC)
    u8[compressed_size] data

[종료]
  u32 0x08E28222  EOFARC   (전역 코멘트가 멤버들 뒤에 올 수 있음 — 확장 필드로 skip)
```

- LZMA 블록 데이터: `u8[2] 예약 + u16 prop_size(=5)` + `u8[prop_size] LZMA props` +
  raw LZMA 스트림(길이 = compressed_size − 4 − prop_size). prop_size 위치는 reference
  디코더 확인(오프셋 2의 u16). 벤더링된 `LzmaDec`로 uncompressed_size까지 디코딩.
- 빈 파일/디렉토리는 file_length 0이고 블록이 없음.
- **solid (0x24E5A060)**: 파일 헤더 전체가 먼저 나오고 하나의 블록 스트림을 공유.
  디코딩 출력을 멤버 크기 순서대로 분배(demux)해 지원 — 멤버 1개만 상주.
- 분할 볼륨(0x24F5A262)·전역 암호화(0x08D144A8)는 아카이브 단위 오류.
