# JDoc 바인딩 & 포맷 판별(`detect`) API

JDoc의 다국어 바인딩과, 5개 언어 공통 포맷 판별 API 사용법.

- **C++ / C API** — `include/jdoc/` 헤더 (코어에 포함)
- **Python** — pybind11 (`import jdoc`)
- **Go** — cgo (`bindings/go/`)
- **Java** — JNA (`bindings/java/`)

Go·Java는 C API를 감싸므로, 먼저 코어를 빌드해 공유 라이브러리
(`libjdoc.dylib` / `libjdoc.so` / `jdoc.dll`)를 만들어야 한다
(`cmake -S . -B build && cmake --build build --target jdoc_shared`).
릴리스에서는 `jdoc-sdk-<platform>` 아티팩트가 공유 라이브러리 + 헤더를 제공한다.

---

## 포맷 판별 API (`detect`)

추출(convert) 없이 파일의 포맷만 판별한다. 매직바이트 + 컨테이너 구조를 검사해
리치 구조체를 돌려주므로, 변환 전에 파일 종류로 분기하거나 지원 여부를 미리
확인할 수 있다.

### 반환 구조체 `FormatInfo`

| 필드 | 설명 | 예 |
|---|---|---|
| `format` | 정규 포맷 이름(대문자 토큰) | `"PDF"`, `"DOCX"`, `"PNG"`, `"UNKNOWN"` |
| `category` | 대분류 | `document`, `spreadsheet`, `presentation`, `archive`, `email`, `text`, `image`, `unknown` |
| `extension` | 정규 확장자(점 포함) | `".pdf"` |
| `mime` | MIME 타입 | `"application/pdf"` |
| `convertible` | jdoc가 텍스트 추출 가능한가 (`convert`/`convert_archive`) | `true`/`false` |

- **이미지(png/jpeg/gif/bmp/tiff/webp/ico/psd)** 는 검출은 되지만 `convertible=false`.
  jdoc는 문서만 변환하므로, 이미지·미지원 파일을 사전에 걸러내는 용도로 쓴다.
- **아카이브(zip/gz/bz2/tar/7z/alz/egg/rar)** 는 `convertible=true` — `convert_archive`로 처리 가능.
- 바이트로 결정되지 않으면 확장자를 폴백으로 쓴다(라이브러리 전반과 동일한 정책).

정밀 포맷 구분(DOCX vs XLSX vs PPTX, HWP vs HWPX 등)은 내부적으로 기존
`detect_office_format` / 컨테이너 판별을 재사용한다.

---

## 언어별 사용법

### C++

```cpp
#include "jdoc/detect.h"

jdoc::FormatInfo info = jdoc::detect("report.pdf");
// info.format == "PDF", info.category == jdoc::FormatCategory::Document,
// info.convertible == true

// 메모리 버퍼
jdoc::FormatInfo m = jdoc::detect(data, size, "name_hint.docx");
```

### C

```c
#include <jdoc/jdoc_c_api.h>

JDocFormatInfo info;
char err[256];
if (jdoc_detect("report.pdf", &info, err, sizeof err) == 0) {
    printf("%s / %d / %s / conv=%d\n",
           info.format, info.category, info.mime, info.convertible);
    jdoc_free_format_info(&info);   // 내부 문자열 해제
}
```

### Python

```python
import jdoc

info = jdoc.detect("report.pdf")
# <FormatInfo format='PDF' category='document' ext='.pdf' convertible=True>
print(info.format, info.category_name, info.extension, info.convertible)

# 바이트
info = jdoc.detect_bytes(open("a.png", "rb").read())
if not info.convertible:
    print("skip:", info.format)
```

### Go

`bindings/go/` — cgo로 C API를 감싼다. 헤더/라이브러리 경로는 cgo 지시문에
잡혀 있으며(`../../include`, `../../build`), 필요 시 `CGO_CFLAGS`/`CGO_LDFLAGS`로
재정의한다. 런타임에 `libjdoc`를 찾도록 Linux/macOS는 rpath가 박혀 있고,
Windows는 `jdoc.dll`을 PATH에 둔다.

```go
import "github.com/jiran/jdoc/bindings/go"

info, err := jdoc.Detect("report.pdf")
// info.Format == "PDF", info.Category == jdoc.CategoryDocument, info.Convertible == true

info, _ = jdoc.DetectBytes(data, "name.docx")
md, _ := jdoc.Convert("report.pdf")
```

빌드·테스트:
```sh
cd bindings/go
DYLD_LIBRARY_PATH=../../build go test ./...   # Linux는 LD_LIBRARY_PATH
```

### Java (JNA)

`bindings/java/` — JNA로 C API에 직접 매핑(네이티브 글루 컴파일 불필요).
`libjdoc`를 `-Djna.library.path=<빌드 디렉터리>`로 지정한다. JNA가 플랫폼별
라이브러리 이름(`libjdoc.dylib`/`libjdoc.so`/`jdoc.dll`)을 자동 처리한다.

```java
import com.jiran.jdoc.*;

FormatInfo info = Jdoc.detect("report.pdf");
// info.format="PDF", info.category=Category.DOCUMENT, info.convertible=true

FormatInfo m = Jdoc.detectBytes(bytes, "name.docx");
String md = Jdoc.convert("report.pdf");
```

빌드·테스트:
```sh
cd bindings/java
gradle test -Djna.library.path=../../build
```

---

## 설계 노트

판별과 추출은 분리돼 있다. `detect`는 헤더 매직바이트 + 컨테이너 구조만 검사하고,
정수 코드가 아니라 이름·카테고리·확장자·MIME·변환가능 여부를 담은 리치 구조체를
5개 언어에서 동일하게 반환한다. 정밀 포맷 구분(DOCX/XLSX/PPTX, HWP/HWPX 등)은
내부 `detect_format` / `detect_office_format` 로직을 재사용한다.
