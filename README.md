# JDoc

C++17 기반 문서 → 마크다운 변환기. 무거운 의존성 없이 zlib, libjpeg-turbo, pugixml만 사용합니다(7z 디코더는 public domain LZMA SDK를 벤더링).

**지원 포맷:** PDF, DOCX, XLSX, XLSB, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX, TXT

**아카이브 (압축 해제 없이 직접 파싱):** ZIP, GZ, BZ2, TAR, TAR.GZ, TAR.BZ2, 7Z, ALZ, EGG

## 주요 특징

- **자체 구현 PDF 파서** — PDFium/Poppler 미사용, 전역 상태 없는 완전한 스레드 안전
- **제목(헤딩) 감지** — 폰트 크기 비율 분석 + 굵은 글씨·절 번호 패턴 인식 (H1–H5)
- **표 추출** — 괘선 기반 그리드 + 무괘선 텍스트 표 감지
- **이미지 추출** — JPEG 패스스루, 150 DPI 벡터 렌더링, CCITTFax G3/G4, 최소 크기 필터
- **한글 문서 지원** — HWP/HWPX 표·이미지·제목 완전 지원, HWP로 생성된 PDF 대응(Type3 폰트, 렌더 모드 기반 굵은 글씨 인식)
- **암호화 PDF** — RC4 표준 보안 핸들러 (40/128비트)
- **손상 PDF 복구** — xref 재구축, 스트림 길이 복구
- **CJK 인코딩** — CP949, CP932, CMap 기반 유니코드 매핑
- **페이지 청킹** — RAG 파이프라인용 페이지별 출력(메타데이터 포함)
- **아카이브 직접 파싱** — ZIP/GZ/BZ2/TAR/TAR.GZ/TAR.BZ2/7Z/ALZ/EGG 내부 문서를 디스크에 풀지 않고 메모리에서 스트리밍 변환. 멤버는 한 번에 하나만 상주하고, 중첩 아카이브는 재귀 처리하며, 손상·미지원 멤버는 해당 멤버만 오류로 기록하고 순회 지속
- **아카이브 코덱** — 7Z: LZMA/LZMA2·branch 필터(디코더 전용 LZMA SDK 벤더링, solid block 사전 크기 검사). ALZ/EGG: store/deflate/bzip2/LZMA, solid EGG 스트리밍 분배 지원, CRC 검증, CP949 파일명 변환. 암호화·독점 코덱(AZO) 멤버는 명확한 오류로 보고
- **압축폭탄 방어** — 헤더 크기 필드를 신뢰하지 않고 해제 도중 출력 바이트를 계수해 강제. 멤버당·누적·멤버 수·압축비·재귀 깊이 한도 (기본값과 해제 방법은 [옵션](#옵션) 참조)
- **단일 스레드** — 변환 호출당 스레드 1개, 전역 상태 없음. 호출자가 문서/아카이브 단위로 자유롭게 병렬화 가능
- **다양한 API** — CLI, Python (pybind11), C, C++

## 설치

시스템 의존성 (최초 1회, C++/Python 빌드 공통):

```bash
# Ubuntu/Debian
sudo apt install cmake build-essential zlib1g-dev libjpeg-dev
# macOS
brew install cmake libjpeg-turbo
# RHEL/Fedora
sudo dnf install cmake gcc-c++ zlib-devel libjpeg-turbo-devel
```

빌드:

```bash
# Python
pip install .

# C++
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

## 사용법

### CLI

```bash
jdoc input.pdf                              # 마크다운을 stdout으로 출력
jdoc input.pdf output.md                    # 파일로 저장
jdoc input.docx --plaintext                 # 일반 텍스트 출력
jdoc input.pdf --pages 0,1,2                # 페이지 선택 (0부터 시작)
jdoc input.pdf --chunks                     # 페이지별 출력
jdoc input.pdf --images ./imgs              # 이미지 추출
jdoc input.pdf --images ./imgs --min-image-size 100   # 100px 미만 이미지 제외
jdoc input.pdf --images ./imgs --min-image-size 0     # 모든 이미지 추출

# 아카이브: 압축 해제 없이 내부 문서를 멤버별로 변환
jdoc docs.zip                               # === 멤버 경로 === 구분으로 출력
jdoc docs.tar.gz --max-depth 2              # 중첩 아카이브 재귀 깊이 제한
jdoc docs.zip --max-member-mb 64            # 멤버당 해제 후 크기 상한(MiB, -1 = 무제한)
jdoc docs.zip --include-unsupported         # 미지원 멤버도 오류로 보고
```

### Python

```python
import jdoc

# 마크다운으로 변환
text = jdoc.convert("document.pdf")
text = jdoc.convert("report.docx", format="text", pages=[0, 1])

# 이미지 포함 페이지별 청크
pages = jdoc.convert_pages("document.pdf", extract_images=True)
for page in pages:
    print(page.text)
    for img in page.images:
        print(f"  {img.name} {img.width}x{img.height} {img.format}")
        # img.data   — JPEG/PNG 바이트
        # img.pixels — 원시 RGB 버퍼 (width * height * components)

# 이미지 크기 필터
text = jdoc.convert("doc.pdf", extract_images=True, min_image_size=100)  # 100px 미만 제외
text = jdoc.convert("doc.pdf", extract_images=True, min_image_size=0)    # 필터 없음

# ConvertOptions로 세부 제어
opts = jdoc.ConvertOptions()
opts.extract_images = True
opts.image_output_dir = "./images"
opts.min_image_size = 50
opts.pages = [0, 1, 2]

# 아카이브: 압축 해제 없이 멤버별 변환
for m in jdoc.convert_archive("docs.zip", max_depth=3):
    if m.ok:
        print(m.member_path, m.format, len(m.markdown))
    else:
        print(m.member_path, "ERROR:", m.error)

# 메모리 버퍼에서 직접 변환 (파일 I/O 없음)
md = jdoc.convert_bytes(open("doc.hwp", "rb").read(), name_hint="doc.hwp")
```

### C++

```cpp
#include <jdoc/jdoc.h>

// 포맷 자동 감지 (PDF, DOCX, XLSX, PPTX, HWP 등)
std::string md = jdoc::convert("input.pdf");
std::string md = jdoc::convert("report.docx");

// 이미지를 디렉터리로 추출
jdoc::ConvertOptions opts;
opts.pages = {0, 1, 2};
opts.extract_images = true;
opts.image_output_dir = "./images";  // 파일로 저장
opts.min_image_size = 50;
std::string md = jdoc::convert("input.pdf", opts);

// 아카이브: 압축 해제 없이 멤버별 변환 (jdoc/archive.h)
for (auto& m : jdoc::convert_archive("docs.zip")) {
    if (m.ok()) { /* m.member_path, m.format, m.markdown */ }
}
// 콜백 방식 — 결과를 누적하지 않아 대형 아카이브에 적합
jdoc::convert_archive("docs.zip", [](jdoc::MemberResult&& m) {
    return true;  // false 반환 시 조기 중단
});

// 이미지를 메모리에 유지한 페이지별 청크
opts.image_output_dir = "";  // 빈 문자열 = 메모리에만 유지
auto chunks = jdoc::convert_chunks("input.pdf", opts);
for (auto& chunk : chunks) {
    // chunk.text, chunk.tables
    // chunk.page_width, chunk.page_height, chunk.body_font_size
    for (auto& img : chunk.images) {
        // img.name, img.width, img.height, img.format
        // img.data — JPEG/PNG 인코딩된 바이트
    }
}
```

CMake:
```cmake
add_subdirectory(jdoc)
target_link_libraries(your_app PRIVATE jdoc_all)
```

### C API

```c
#include <jdoc/jdoc_c_api.h>

char err[256];

// 단순 텍스트 변환
char* text = jdoc_convert("input.pdf", NULL, err, sizeof(err));
// text 사용...
jdoc_free_string(text);

// 이미지 포함 페이지별 청크
JDocOptions opts = jdoc_default_options();
opts.extract_images = 1;
opts.image_output_dir = "./images";  // NULL = 메모리에만 유지

int page_count;
JDocPage* pages = jdoc_convert_pages("input.pdf", &opts, &page_count, err, sizeof(err));
for (int i = 0; i < page_count; i++) {
    printf("Page %d: %s\n", pages[i].page_number, pages[i].text);
    for (int j = 0; j < pages[i].image_count; j++) {
        JDocImage* img = &pages[i].images[j];
        // img->name, img->width, img->height, img->format
        // img->data (원시 바이트), img->data_size
        // img->saved_path (image_output_dir 설정 시)
    }
}
jdoc_free_pages(pages, page_count);
```

## 옵션

모든 API가 같은 옵션을 공유한다 (C++ `ConvertOptions`, C `JDocOptions`, Python 키워드 인자, CLI 플래그).

### 변환 옵션

| 옵션 | 기본값 | 설명 | CLI |
|---|---|---|---|
| `pages` | 전체 | 추출할 페이지 번호 목록 (0부터 시작) | `--pages 0,1,2` |
| `extract_tables` | `true` | 표를 마크다운 표로 추출 | `--no-tables` (비활성화) |
| `page_chunks` | `false` | 페이지/슬라이드/시트별 청크로 출력 | `--chunks` |
| `extract_images` | `false` | 이미지 추출 | `--images DIR` |
| `image_output_dir` | `""` (메모리 유지) | 이미지 저장 디렉토리. 빈 값이면 바이트로만 반환 | `--images DIR` |
| `image_ref_prefix` | `""` | 마크다운 이미지 참조 경로 앞에 붙일 접두사 | — |
| `min_image_size` | `50` | N×N px 미만 이미지 제외 (`0` = 필터 없음) | `--min-image-size N` |
| `output_format` | `markdown` | `markdown` 또는 `plaintext` | `--plaintext` |

### 아카이브 한도 (`ConvertOptions::archive`)

해제 도중 실시간으로 강제되며, 헤더의 크기 필드는 신뢰하지 않는다. **-1 = 무제한**(음수 전반) — 해당 폭탄 방어가 함께 꺼지므로 신뢰할 수 있는 입력에만 사용.

| 옵션 | 기본값 | 설명 | CLI |
|---|---|---|---|
| `max_depth` | `3` | 중첩 아카이브 재귀 깊이 (최상위 = 1) | `--max-depth N` |
| `max_member_bytes` | `512MiB` | 멤버당 해제 후 크기 상한. 멤버가 하나씩만 상주하므로 **실질 메모리 상한** | `--max-member-mb N` |
| `max_total_bytes` | `64GiB` | 호출당 누적 해제 크기 (CPU 시간 가드) | — |
| `max_entries` | `200000` | 방문 멤버 수 상한 (중첩 포함) | — |
| `max_ratio` | `1000` | 압축비 폭탄 의심 한도 (`0` = 검사 안 함, C++ 전용) | — |
| `include_unsupported` | `false` | 미지원 멤버도 결과에 오류로 포함 | `--include-unsupported` |

- 멤버당·압축비·깊이 초과는 **해당 멤버만 스킵**하고 순회를 계속하며, 누적·멤버 수 초과만 순회를 중단
- 아카이브 멤버의 이미지는 멤버 간 파일명 충돌을 막기 위해 `image_output_dir/<멤버 경로>/` 하위에 저장되며(중첩 구조 보존), 마크다운 참조 경로도 함께 조정됨
- C API는 각 필드에 `0` = 라이브러리 기본값, `-1` = 무제한, 양수 = 지정 값
- bzip2(단독 BZ2/TAR.BZ2, ALZ/EGG 일부 멤버)는 `-DJDOC_WITH_BZIP2=ON` 빌드에서 지원. 기본 OFF 시 해당 멤버만 오류로 보고

## 포맷별 지원 범위

| 기능 | PDF | DOCX | DOC | XLSX/XLSB | XLS | PPTX | PPT | HWP/HWPX | RTF | HTML | TXT |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 텍스트 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| 제목 | ✓ | ✓ | ✓ | | | ✓ | ✓ | ✓ | | | |
| 굵게/기울임 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | |
| 표 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | | ✓ | ✓ | ✓ | |
| 이미지 | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | |
| 목록 | | ✓ | ✓ | | | | | | | ✓ | |
| 링크 | ✓ | ✓ | ✓ | | | | | | | ✓ | |
| 주석 | ✓ | | | | | | | | | | |
| 차트/SmartArt | | | | | | ✓ | | | | | |
| 발표자 노트 | | | | | | ✓ | ✓ | | | | |

## 의존성

| 라이브러리 | 라이선스 | 역할 |
|---|---|---|
| zlib | zlib | 압축 (FlateDecode, PNG, deflate 아카이브 멤버) |
| libjpeg-turbo | IJG/BSD | PDF 이미지 JPEG 디코딩 |
| pugixml | MIT | XML 파싱 (번들 포함) |
| LZMA SDK | public domain | 7z 컨테이너·LZMA 디코딩 (번들 포함, 디코더 전용) |
| libbz2 | BSD | BZ2/TAR.BZ2, ALZ/EGG bzip2 멤버 (선택, `JDOC_WITH_BZIP2`) |
| pybind11 | BSD-3 | Python 바인딩 (선택) |

## 지원 플랫폼

Linux (x64), macOS (arm64/x64), Windows (x64)

## 라이선스

MIT
