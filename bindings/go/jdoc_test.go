package jdoc

import (
	"path/filepath"
	"runtime"
	"testing"
)

// repoRoot resolves the JDoc checkout root from this test file's location
// (bindings/go/jdoc_test.go → ../../).
func repoRoot() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..")
}

func TestDetectPDF(t *testing.T) {
	info, err := Detect(filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"))
	if err != nil {
		t.Fatalf("Detect: %v", err)
	}
	if info.Format != "PDF" {
		t.Errorf("format = %q, want PDF", info.Format)
	}
	if info.Category != CategoryDocument {
		t.Errorf("category = %v, want document", info.Category)
	}
	if !info.Convertible {
		t.Error("PDF should be convertible")
	}
}

func TestDetectArchive(t *testing.T) {
	info, err := Detect(filepath.Join(repoRoot(), "test/fixtures/7z/store.7z"))
	if err != nil {
		t.Fatalf("Detect: %v", err)
	}
	if info.Format != "7Z" || info.Category != CategoryArchive {
		t.Errorf("got %q/%v, want 7Z/archive", info.Format, info.Category)
	}
}

func TestDetectBytesImage(t *testing.T) {
	png := append([]byte{0x89}, []byte("PNG\r\n\x1a\n")...)
	png = append(png, make([]byte, 8)...)
	info, err := DetectBytes(png, "")
	if err != nil {
		t.Fatalf("DetectBytes: %v", err)
	}
	if info.Format != "PNG" || info.Category != CategoryImage || info.Convertible {
		t.Errorf("got %q/%v/conv=%v, want PNG/image/false",
			info.Format, info.Category, info.Convertible)
	}
}

func TestDetectBytesText(t *testing.T) {
	info, err := DetectBytes([]byte("hello world\n"), "readme.txt")
	if err != nil {
		t.Fatalf("DetectBytes: %v", err)
	}
	if info.Format != "TXT" || info.Category != CategoryText {
		t.Errorf("got %q/%v, want TXT/text", info.Format, info.Category)
	}
}

// StreamPages must yield exactly what ConvertPages returns.
func TestStreamPagesEquivalence(t *testing.T) {
	path := filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf")

	eager, err := ConvertPages(path)
	if err != nil {
		t.Fatalf("ConvertPages: %v", err)
	}

	var streamed []Page
	st := StreamPages(path)
	for p := range st.Pages() {
		streamed = append(streamed, p)
	}
	if err := st.Err(); err != nil {
		t.Fatalf("StreamPages: %v", err)
	}

	if len(eager) != len(streamed) {
		t.Fatalf("page count: eager=%d streamed=%d", len(eager), len(streamed))
	}
	for i := range eager {
		if eager[i].PageNumber != streamed[i].PageNumber || eager[i].Text != streamed[i].Text {
			t.Errorf("page %d differs between eager and streamed", i)
		}
	}
}

// Breaking out of the range loop must stop the walk without error.
func TestStreamPagesEarlyStop(t *testing.T) {
	path := filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf")
	seen := 0
	for range StreamPages(path).Pages() {
		seen++
		break
	}
	if seen != 1 {
		t.Errorf("expected to stop after 1 page, saw %d", seen)
	}
}

// A missing file surfaces via Err, and yields no pages.
func TestStreamPagesError(t *testing.T) {
	st := StreamPages(filepath.Join(repoRoot(), "does-not-exist.pdf"))
	n := 0
	for range st.Pages() {
		n++
	}
	if n != 0 {
		t.Errorf("expected 0 pages, got %d", n)
	}
	if st.Err() == nil {
		t.Error("expected an error for a missing file")
	}
}
