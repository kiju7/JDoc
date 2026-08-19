# JDoc

C++17 기반 문서 → 마크다운 변환기. 무거운 의존성 없이 zlib, libjpeg-turbo, pugixml만 사용합니다(7z 디코더는 public domain LZMA SDK를 벤더링).

**지원 포맷:** PDF, DOCX, XLSX, XLSB, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX, TXT

**아카이브 (압축 해제 없이 직접 파싱):** ZIP, GZ, BZ2, TAR, TAR.GZ, TAR.BZ2, 7Z, ALZ, EGG, RAR(store 멤버)

## 주요 특징

- **자체 구현 PDF 파서** — PDFium/Poppler 미사용, 동시 변환을 지원하는 스레드 안전 설계
- **제목(헤딩) 감지** — 폰트 크기 비율 분석 + 굵은 글씨·절 번호 패턴 인식 (H1–H5)
- **표 추출** — 괘선 기반 그리드 + 무괘선 텍스트 표 감지
- **이미지 추출** — JPEG 패스스루, 150~300 DPI 적응형 벡터 렌더링, CCITTFax G3/G4, 최소 크기 필터
- **한글 문서 지원** — HWP/HWPX 표·이미지·제목 완전 지원, HWP 3.x 텍스트 추출, HWP로 생성된 PDF 대응(Type3 폰트, 렌더 모드 기반 굵은 글씨 인식)
- **암호화 PDF** — RC4 표준 보안 핸들러 (40/128비트)
- **손상 PDF 복구** — xref 재구축, 스트림 길이 복구
- **CJK 인코딩** — CP949, CP932, CMap 기반 유니코드 매핑
- **페이지 청킹** — RAG 파이프라인용 페이지별 출력(메타데이터 포함)
- **아카이브 직접 파싱** — ZIP/GZ/BZ2/TAR/TAR.GZ/TAR.BZ2/7Z/ALZ/EGG/RAR 내부 문서를 디스크에 풀지 않고 메모리에서 스트리밍 변환. 멤버는 한 번에 하나만 상주하고, 중첩 아카이브는 재귀 처리하며, 손상·미지원 멤버는 해당 멤버만 오류로 기록하고 순회 지속
- **아카이브 코덱** — 7Z: LZMA/LZMA2/PPMd·branch 필터(디코더 전용 LZMA SDK 벤더링, solid block 사전 크기 검사). ALZ/EGG: store/deflate/bzip2/LZMA, solid EGG 스트리밍 분배 지원, CRC 검증, CP949 파일명 변환. RAR: 4.x/5.x 헤더 워크·store 멤버(독점 압축 코덱 멤버는 멤버별 오류, [근거](docs/rar-feasibility.md)). 암호화·독점 코덱(AZO) 멤버는 명확한 오류로 보고
- **압축폭탄 방어** — 헤더 크기 필드를 신뢰하지 않고 해제 도중 출력 바이트를 계수해 강제. 멤버당·누적·멤버 수·압축비·재귀 깊이 한도 (기본값과 해제 방법은 [옵션](#옵션) 참조)
- **제한된 페이지 병렬화** — PDF는 사용 가능한 CPU 범위에서 페이지를 병렬 처리하고 합성 렌더 작업 메모리는 프로세스 전체 256 MiB로 제한. 다른 포맷은 호출당 단일 스레드이며 문서 단위 병렬 호출 가능
- **다양한 API** — CLI, Python (pybind11), C, C++, Go (cgo), Java (JNA)
- **포맷 판별** — 추출 없이 파일 포맷만 판별하는 `detect` API. 이름·카테고리·확장자·MIME·변환가능 여부를 담은 구조체 반환, 이미지 등 비변환 포맷도 검출. 5개 언어 공통

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
jdoc input.docx --format text               # 일반 텍스트 출력
jdoc input.pdf --pages 0,1,2                # 페이지 선택 (0부터 시작)
jdoc input.pdf --chunks                     # 페이지별 출력
jdoc input.pdf --images ./imgs              # 기본 추출 이미지를 디스크에도 저장
jdoc input.pdf --no-images                   # 이미지 추출 비활성화
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

# 이미지 포함 페이지별 청크 (이미지 추출은 기본값)
pages = jdoc.convert_pages("document.pdf")
for page in pages:
    print(page.text)
    for img in page.images:
        print(f"  {img.name} {img.width}x{img.height} {img.format}")
        # img.data   — JPEG/PNG 바이트
        # img.pixels — 원시 RGB 버퍼 (width * height * components)

# 이미지 크기 필터
text = jdoc.convert("doc.pdf", images=True, min_image_size=100)  # 100px 미만 제외
text = jdoc.convert("doc.pdf", images=True, min_image_size=0)    # 필터 없음

# ConvertOptions로 세부 제어
opts = jdoc.ConvertOptions()
opts.images = True
opts.image_dir = "./images"
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
pages = jdoc.convert_pages_bytes(data, name_hint="report.pdf")
for page in jdoc.convert_pages_bytes_stream(data, name_hint="report.pdf"):
    process(page)
```

### C++

```cpp
#include <jdoc/jdoc.h>

// 포맷 자동 감지 (PDF, DOCX, XLSX, PPTX, HWP 등)
std::string md = jdoc::convert("input.pdf");
std::string md = jdoc::convert("report.docx");

// 이미지 추출은 기본값. 디렉터리를 지정하면 파일로도 저장
jdoc::ConvertOptions opts;
opts.pages = {0, 1, 2};
opts.image_dir = "./images";  // 파일로 저장
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
opts.image_dir = "";  // 빈 문자열 = 메모리에만 유지
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
target_link_libraries(your_app PRIVATE jdoc_lib)  # 단일 정적 라이브러리 libjdoc.a
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
opts.image_dir = "./images";  // NULL = 메모리에만 유지

int page_count;
JDocPage* pages = jdoc_convert_pages("input.pdf", &opts, &page_count, err, sizeof(err));
for (int i = 0; i < page_count; i++) {
    printf("Page %d: %s\n", pages[i].page_number, pages[i].text);
    for (int j = 0; j < pages[i].image_count; j++) {
        JDocImage* img = &pages[i].images[j];
        // img->name, img->width, img->height, img->format
        // img->data (JPEG/PNG 등 인코딩 바이트), img->data_size
        // img->pixels (원시 픽셀), img->pixels_size, img->components
        // img->saved_path (image_dir 설정 시)
    }
}
jdoc_free_pages(pages, page_count);
```

### 스트리밍 (지연 페이지 이터레이터)

`convert_pages`/`convert_chunks`는 전 페이지를 한 번에 만들어 반환한다(eager). 스트리밍 API는 **페이지를 하나씩 생성→소비→해제**해, 소비자가 한 페이지분 메모리만 들고 큰 문서를 처리할 수 있다. 출력은 eager와 **바이트 동일**하며, 콜백이 `false`(C는 `0`)를 반환하면 조기 중단한다.

pptx(슬라이드)·xlsx(시트)·hwp/hwpx(섹션)처럼 단위별로 지연 파싱되는 포맷은 **첫 페이지 지연과 피크 메모리가 크게 준다** (예: 25슬라이드 pptx에서 첫 페이지 ≈−98%, 피크 ≈−44%). PDF·docx 등 문서 전체를 한 번에 파싱하는 포맷도 API는 동일하게 동작하며 소비자측 페이지 단위 소유 이득을 준다.

```python
# Python — 제너레이터
for page in jdoc.convert_pages_stream("big.pptx", images=True):
    process(page)          # 한 페이지씩 지연 생성; break로 조기 중단
```

```cpp
// C++ — sink 콜백 (jdoc/jdoc.h)
jdoc::for_each_chunk("big.pptx", opts, [](jdoc::PageChunk&& page) {
    // page.text, page.images ...
    return true;           // false 반환 시 조기 중단
});
```

```c
/* C API — 콜백형 (jdoc/jdoc_c_api.h). page는 콜백 동안만 유효 */
int on_page(const JDocPage* page, void* userdata) {
    printf("Page %d: %s\n", page->page_number, page->text);
    return 1;              /* 0 반환 시 조기 중단 */
}
jdoc_convert_pages_stream("big.pptx", &opts, on_page, NULL, err, sizeof(err));
```

Go(`StreamPages(path).Pages()` → `iter.Seq[Page]`)와 Java(`Jdoc.streamPages(path)` → `Iterable<Page>`) 바인딩도 각 언어의 지연 이터레이터로 같은 API를 제공한다 (`bindings/go`, `bindings/java`). 메모리 입력은 각각 `StreamPagesBytes`와 `streamPagesBytes`를 사용한다.

## API 일관성과 스레드 안전성

공개 API는 언어별 명명 관례만 다르고 입력 종류, 옵션, 페이지 결과의 의미는 같다. 모든 페이지 결과는 `page_number`, 텍스트, 페이지 크기, 본문 폰트 크기, 표, 이미지, 저품질 이미지 수를 제공하며 페이지 번호는 항상 **0부터 시작**한다.

| 작업 | C++ | C | Python | Go | Java |
|---|---|---|---|---|---|
| 파일/메모리 포맷 판별 | `detect` | `jdoc_detect[_mem]` | `detect[_bytes]` | `Detect[Bytes]` | `detect[Bytes]` |
| 파일/메모리 전체 변환 | `convert` 오버로드 | `jdoc_convert[_mem]` | `convert[_bytes]` | `Convert[Bytes]` | `convert[Bytes]` |
| 파일/메모리 페이지 목록 | `convert_chunks` 오버로드 | `jdoc_convert_pages[_mem]` | `convert_pages[_bytes]` | `ConvertPages[Bytes]` | `convertPages[Bytes]` |
| 파일/메모리 페이지 스트림 | `for_each_chunk` 오버로드 | `jdoc_convert_pages[_mem]_stream` | `convert_pages[_bytes]_stream` | `StreamPages[Bytes]` | `streamPages[Bytes]` |
| 아카이브 멤버 변환 | `convert_archive` | `jdoc_convert_archive` | `convert_archive` | `ConvertArchive` | `convertArchive` |

스레드 안전성 계약은 다음과 같다.

- 서로 다른 문서에 대한 `detect`, `convert`, 페이지 변환, 아카이브 변환 호출은 같은 프로세스에서 동시에 실행할 수 있다. 호출 중 입력 버퍼와 옵션은 변경하지 않아야 한다.
- 하나의 페이지 스트림은 단일 소비자용이며 한 번만 순회할 수 있다. Python/Java 스트림은 사용 후 닫고, Go는 순회 후 `Err()`를 확인한다.
- PDF 내부 페이지 병렬 처리와 문서 단위 병렬 호출을 함께 사용할 수 있다. 합성 렌더 작업은 프로세스 전체 메모리 게이트로 제한된다.
- 여러 변환이 동일한 `image_dir`와 파일명을 사용해도 기존 파일을 덮어쓰지 않고 `_1`, `_2` 접미사를 붙인다. 파일 생성이 원자적(POSIX `O_EXCL`, Windows `CREATE_NEW`)이므로 이 보장은 스레드뿐 아니라 **프로세스 간에도 성립**한다.
- C 스트리밍 콜백의 `JDocPage*`만 콜백 동안 빌린 값이다. 나머지 언어 바인딩은 페이지와 이미지 바이트를 각 언어 소유 메모리로 복사한다.
- 경로 문자열은 모든 API에서 UTF-8이며 Windows에서도 한글 등 비 ASCII 경로를 지원한다.

## 옵션

C++ `ConvertOptions`와 Python `ConvertOptions`는 같은 필드를 공유한다. C API와
CLI는 각 환경의 관례에 맞는 대응 필드·플래그를 제공한다.

### 변환 옵션

| 옵션 | 기본값 | 설명 | CLI |
|---|---|---|---|
| `pages` | 전체 | 추출할 페이지 번호 목록 (0부터 시작) | `--pages 0,1,2` |
| `tables` | `true` | 표를 마크다운 표로 추출 | `--no-tables` (비활성화) |
| `page_chunks` | `false` | 페이지/슬라이드/시트별 청크로 출력 | `--chunks` |
| `images` | `true` | 이미지 추출 | `--no-images` (비활성화) |
| `image_dir` | `""` (메모리 유지) | 이미지 저장 디렉토리. 빈 값이면 바이트로만 반환 | `--images DIR` |
| `image_ref_prefix` | `""` | 마크다운 이미지 참조 경로 앞에 붙일 접두사 | — |
| `min_image_size` | `50` | 문서에 포함된 이미지 중 N×N px 미만 제외 (`0` = 필터 없음). 이미지 파일 자체를 변환하는 경우에는 적용되지 않음 | `--min-image-size N` |
| `format` | `markdown` | `markdown` 또는 `text` | `--format F` |

### 아카이브 한도 (`ConvertOptions::archive`)

해제 도중 실시간으로 강제되며, 헤더의 크기 필드는 신뢰하지 않는다. **-1 = 무제한**(음수 전반) — 해당 폭탄 방어가 함께 꺼지므로 신뢰할 수 있는 입력에만 사용.

| 옵션 | 기본값 | 설명 | CLI |
|---|---|---|---|
| `max_depth` | `3` | 중첩 아카이브 재귀 깊이 (최상위 = 1) | `--max-depth N` |
| `max_member_bytes` | `512MiB` | 멤버당 해제 후 크기 상한. 멤버가 하나씩만 상주하므로 **실질 메모리 상한** | `--max-member-mb N` |
| `max_total_bytes` | `64GiB` | 호출당 누적 해제 크기 (CPU 시간 가드) | `--max-total-mb N` |
| `max_entries` | `200000` | 방문 멤버 수 상한 (중첩 포함) | `--max-entries N` |
| `max_ratio` | `10000` | 압축비 폭탄 의심 한도 (`0` = 검사 안 함) | `--max-ratio N` |
| `include_unsupported` | `false` | 미지원 멤버도 결과에 오류로 포함 | `--include-unsupported` |

- 멤버당·압축비·깊이 초과는 **해당 멤버만 스킵**하고 순회를 계속하며, 누적·멤버 수 초과만 순회를 중단
- 아카이브 멤버의 이미지는 멤버 간 파일명 충돌을 막기 위해 `image_dir/<멤버 경로>/` 하위에 저장되며(중첩 구조 보존), 마크다운 참조 경로도 함께 조정됨
- 동일 출력 파일명이 이미 있으면 덮어쓰지 않고 `_1`, `_2` 접미사를 붙이며 실제 저장 경로가 결과와 마크다운에 반영됨
- 추출 이미지의 확장자는 **컨테이너가 선언한 것을 그대로** 쓴다. zip 엔트리명이나 HWP BinData처럼 이름으로 선언된 경우 `.jpeg`는 `.jpeg`, `.webp`는 `.webp`로 저장된다. Escher BLIP 레코드나 PDF 필터처럼 타입 코드만 선언하는 경우에만 포맷의 표준 확장자를 쓴다
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
| 발표자 노트 | | | | | | ✓ | ✓ | | | | |

HTML의 이미지는 문서가 실제로 담고 있는 `data:` URI만 추출한다. `http(s)` 주소나 상대 경로는 문서 밖 파일을 가리키므로 저장하지 않고 원본 참조를 그대로 둔다(네트워크 요청을 하지 않는다).

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
