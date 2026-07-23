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
