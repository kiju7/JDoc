# ALZ / EGG 포맷 정리 (P3 구현 근거)

구현 근거 문서. 출처: EggDotNet(MIT, github.com/akolman/EggDotNet)의 포맷 상수·레이아웃,
reference 디컴파일 스케치(시그니처·메서드 코드 교차확인용), ESTsoft 공개 명세 요지.
GPL 구현(unalz 등)은 참조하지 않음. 모든 정수는 little-endian.

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
  u16 bit_flags   — bit0: 암호화(데이터 앞 12바이트 암호 헤더 추가)
                    (bit_flags & 0xF0) >> 4 = 크기 필드 폭 w ∈ {1,2,4,8}
  [bit_flags != 0 인 경우만]
    u16 method    0=store, 1=bzip2, 2=deflate(raw)
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
공통적으로 `u8 bit_flag + u16 size + u8[size] data` 형태(안전한 skip 가능).

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
    0x0A8591AC filename: u8 flags + u16 size + [flags&8: u16 locale] + u8[size] name(UTF-8)
                         flags&4 = 파일명 암호화(미지원)
    0x2C86950B win file info   (flag+size+data, 총 12바이트 본문)
    0x1EE922E5 posix file info
    0x08D1470F encrypt header  → 해당 멤버 암호화(미지원 오류)
    0x04C63672 comment
    0x07463307 dummy
  u32 0x08E28222  EOFARC — 파일 헤더 구간 종료
  [블록]*         — file_length를 채울 때까지 반복 (대부분 1블록)
    u32 0x02B50C13 block header magic
    u16 method     하위바이트: 0=store, 1=deflate(raw), 2=bzip2, 3=AZO(독점, 미지원), 4=lzma
                   상위바이트: hint
    u32 uncompressed_size
    u32 compressed_size
    u32 crc32
    u32 0x08E28222 EOFARC — 블록 헤더 종료
    u8[compressed_size] data

[종료]
  u32 0x08E28222  EOFARC
```

- LZMA 블록 데이터: `u8[4] (버전/예약, 무시)` + `u8[5] LZMA props` + raw LZMA 스트림
  (스트림 길이 = compressed_size − 9). 벤더링된 `LzmaDec`로 uncompressed_size까지 디코딩.
- 빈 파일/디렉토리는 file_length 0이고 블록이 없음.
- solid: 파일 헤더들이 먼저 쭉 나오고 블록 스트림을 공유 — 미지원(아카이브 단위 오류).
