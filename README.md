# JDoc

C++17 기반 문서 → 마크다운 변환기. 무거운 의존성 없이 zlib, libjpeg-turbo, pugixml만 사용합니다(7z 디코더는 public domain LZMA SDK를 벤더링).

**지원 포맷:** PDF, DOCX, XLSX, XLSB, PPTX, DOC, XLS, PPT, RTF, HTML, HWP, HWPX, TXT

**아카이브 (압축 해제 없이 직접 파싱):** ZIP, GZ, TAR, TAR.GZ, 7Z, ALZ, EGG

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
- **아카이브 직접 파싱** — ZIP/GZ/TAR/TAR.GZ/7Z/ALZ/EGG 내부 문서를 디스크에 풀지 않고 메모리에서 스트리밍 변환. 멤버는 한 번에 하나만 메모리에 상주하며, 중첩 아카이브 재귀(깊이 제한)·압축폭탄 방어(해제 중 크기·비율 강제)·CP949 파일명 변환 지원. 단일 스레드로 동작하며 호출자가 문서 단위로 병렬화 가능
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
jdoc docs.zip                               # === 경로 (포맷) === 구분으로 출력
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

## 아카이브 처리

ZIP/GZ/TAR/TAR.GZ/7Z/ALZ/EGG 아카이브를 디스크에 풀지 않고 내부 문서를 변환한다.

- **메모리 상주 최소화** — 멤버를 한 번에 하나만 스트리밍 해제(64KB 청크)해 메모리 버퍼에서 파싱하고, 다음 멤버 전에 해제. tar.gz는 컨테이너 자체도 스트리밍 순회(gzip inflate → tar 헤더 파싱)라 전체가 메모리에 올라가지 않음. 7z는 solid block 단위로 디코딩하므로 피크 메모리가 solid block 크기와 같고, 디코딩 전에 block 크기를 멤버 상한과 비교해 초과분은 할당 없이 스킵
- **7z 코덱** — LZMA/LZMA2/store 및 branch 필터(BCJ/BCJ2/delta 등) 지원, 디코더 전용 LZMA SDK 벤더링(`vendor/lzma/`). 암호화 멤버·PPMd는 멤버별 오류로 보고
- **국산 포맷(ALZ/EGG)** — ALZ: store/deflate/bzip2, CP949 파일명 변환, 멤버 CRC 검증. EGG: store/deflate/bzip2/LZMA(벤더링 LzmaDec 재사용), 멀티블록 멤버, 블록별 CRC 검증. 암호화 멤버·AZO(독점 코덱)·solid/분할 egg는 명확한 오류로 보고. bzip2 멤버는 `-DJDOC_WITH_BZIP2=ON`(시스템 libbz2) 빌드에서 지원, 기본 OFF 시 해당 멤버만 오류
- **압축폭탄 방어** — 헤더의 크기 필드를 신뢰하지 않고 해제 도중 출력 바이트를 계수해 강제. 기본값: 멤버당 512MiB(실질 메모리 상한), 호출당 누적 64GiB·멤버 수 200,000(CPU 시간 가드 — 메모리와 무관), 압축비 1000:1, 재귀 깊이 3 (`ConvertOptions::archive`). 멤버당·압축비·깊이 초과는 해당 멤버만 스킵하고 순회를 계속하며, 누적·멤버 수 초과만 순회를 중단. 각 한도는 **-1로 무제한 해제 가능**(C API·CLI·Python 공통) — 신뢰할 수 있는 입력에만 사용 권장
- **단일 스레드** — 변환 호출당 스레드 1개. 전역 상태가 없어 호출자가 문서/아카이브 단위로 자유롭게 병렬화 가능
- **한글 파일명** — UTF-8 플래그가 없는 레거시 ZIP의 CP949 파일명을 UTF-8로 변환
- **관대한 실패 처리** — 손상 멤버는 해당 멤버만 오류로 기록하고 순회 지속. macOS 메타데이터(`__MACOSX/`, `._*`, `.DS_Store`)는 자동 제외

### 벤치마크 — 압축해제 후 파싱 대비

Apple Silicon macOS, 실문서 코퍼스 기준. A = 압축 해제 후 파일별 변환(임시 디렉토리 왕복), B = `convert_archive()` 직접 파싱. 동일 픽스처에서 변환 결과(멤버 수·출력 바이트)는 두 방식이 일치.

| 픽스처 | A: 해제 후 파싱 | B: 직접 파싱 | 속도 향상 | B 최대 메모리 |
|---|---|---|---|---|
| 문서 203개 zip (80MB) | 1.29s | 0.45s | **2.9×** | 68MB |
| 문서 203개 tar.gz (81MB) | 1.12s | 0.47s | **2.4×** | 106MB |
| 대용량 단일 zip (144MB) | 1.31s | 0.37s | **3.5×** | 144MB¹ |
| 대용량 단일 tar.gz (145MB) | 0.57s | 0.18s | **3.1×** | 144MB¹ |

¹ 멤버가 메모리에 실체화되므로 최대 메모리 ≈ 멤버 해제 크기(상한은 `max_member_bytes`로 제어). 방식 A는 같은 데이터를 디스크에 쓰고 다시 읽는 비용을 치른다.

## 의존성

| 라이브러리 | 라이선스 | 역할 |
|---|---|---|
| zlib | zlib | 압축 (FlateDecode, PNG) |
| libjpeg-turbo | IJG/BSD | PDF 이미지 JPEG 디코딩 |
| pugixml | MIT | XML 파싱 (번들 포함) |
| pybind11 | BSD-3 | Python 바인딩 (선택) |

## 지원 플랫폼

Linux (x64), macOS (arm64/x64), Windows (x64)

## 라이선스

MIT
