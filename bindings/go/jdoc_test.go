package jdoc

import (
	"os"
	"path/filepath"
	"runtime"
	"sync"
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

func TestDefaultOptionsExtractImages(t *testing.T) {
	if !DefaultOptions().Images {
		t.Error("image extraction must be enabled by default")
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

func TestEmptyBytesAreAnEmptyTextDocument(t *testing.T) {
	info, err := DetectBytes(nil, "empty.txt")
	if err != nil || info.Format != "TXT" {
		t.Fatalf("DetectBytes(empty): info=%+v err=%v", info, err)
	}
	text, err := ConvertBytes(nil, "empty.txt")
	if err != nil || text != "" {
		t.Fatalf("ConvertBytes(empty): text=%q err=%v", text, err)
	}
	pages, err := ConvertPagesBytes(nil, "empty.txt")
	if err != nil || len(pages) != 1 || pages[0].Text != "" {
		t.Fatalf("ConvertPagesBytes(empty): pages=%+v err=%v", pages, err)
	}
}

func TestInvalidOptionsAreRejected(t *testing.T) {
	opts := DefaultOptions()
	opts.Pages = []int{-1}
	if _, err := ConvertWithOptions(
		filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"), opts); err == nil {
		t.Error("negative page must be rejected")
	}
	opts = DefaultOptions()
	opts.Format = "html"
	if _, err := ConvertWithOptions(
		filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"), opts); err == nil {
		t.Error("unknown output format must be rejected")
	}
}

func TestConvertBytesText(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"))
	if err != nil {
		t.Fatal(err)
	}
	got, err := ConvertBytes(data, "sample.pdf")
	if err != nil {
		t.Fatalf("ConvertBytes: %v", err)
	}
	want, err := Convert(filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"))
	if err != nil {
		t.Fatalf("Convert: %v", err)
	}
	if got != want {
		t.Error("ConvertBytes output differs from Convert")
	}
}

func TestConvertPagesBytes(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"))
	if err != nil {
		t.Fatal(err)
	}
	pages, err := ConvertPagesBytes(data, "sample.pdf")
	if err != nil {
		t.Fatalf("ConvertPagesBytes: %v", err)
	}
	if len(pages) == 0 || pages[0].PageWidth <= 0 || pages[0].PageHeight <= 0 {
		t.Fatalf("missing page metadata: %+v", pages)
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

func TestStreamPagesBytesEquivalence(t *testing.T) {
	data, err := os.ReadFile(filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"))
	if err != nil {
		t.Fatal(err)
	}
	eager, err := ConvertPagesBytes(data, "sample.pdf")
	if err != nil {
		t.Fatal(err)
	}
	var streamed []Page
	st := StreamPagesBytes(data, "sample.pdf")
	for page := range st.Pages() {
		streamed = append(streamed, page)
	}
	if err := st.Err(); err != nil {
		t.Fatal(err)
	}
	if len(eager) != len(streamed) {
		t.Fatalf("page count: eager=%d streamed=%d", len(eager), len(streamed))
	}
	for i := range eager {
		if eager[i].PageNumber != streamed[i].PageNumber || eager[i].Text != streamed[i].Text {
			t.Errorf("page %d differs between eager and streamed bytes", i)
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

func TestStreamPagesIsSingleUse(t *testing.T) {
	st := StreamPages(filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf"))
	for range st.Pages() {
		// drain the first and only walk
	}
	for range st.Pages() {
		t.Fatal("a second Pages iteration must not yield values")
	}
	if st.Err() == nil {
		t.Error("expected a single-use error on the second iteration")
	}
}

func TestConcurrentConvert(t *testing.T) {
	path := filepath.Join(repoRoot(), "test/fixtures/pdf/sample.pdf")
	const workers = 8
	var wg sync.WaitGroup
	errs := make(chan error, workers)
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, err := Convert(path)
			errs <- err
		}()
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			t.Errorf("concurrent Convert: %v", err)
		}
	}
}
