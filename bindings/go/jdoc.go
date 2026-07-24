// Package jdoc is a Go binding for the JDoc document converter, built on the
// C API (include/jdoc/jdoc_c_api.h) via cgo.
//
// Build/link: the cgo directives below expect the JDoc headers under
// ../../include and the shared library (libjdoc.dylib/.so / jdoc.dll) under
// ../../build. Override with CGO_CFLAGS / CGO_LDFLAGS if your layout differs:
//
//	CGO_CFLAGS="-I/opt/jdoc/include" \
//	CGO_LDFLAGS="-L/opt/jdoc/lib -ljdoc" go build ./...
//
// Runtime library lookup is handled per-OS:
//   - Linux / macOS: an rpath to ../../build is baked in, so `go test` in this
//     directory works with no env vars. For an installed lib, an rpath to a
//     standard location (or LD_LIBRARY_PATH / DYLD_LIBRARY_PATH) also works.
//   - Windows: rpath does not exist; put jdoc.dll on PATH or next to the .exe.
package jdoc

/*
#cgo CFLAGS: -I${SRCDIR}/../../include -Wno-nullability-completeness
#cgo LDFLAGS: -L${SRCDIR}/../../build -ljdoc
#cgo linux LDFLAGS: -Wl,-rpath,${SRCDIR}/../../build
#cgo darwin LDFLAGS: -Wl,-rpath,${SRCDIR}/../../build
#include <stdlib.h>
#include <jdoc/jdoc_c_api.h>

// Bridge: jdoc_convert_pages_stream takes a C function pointer. goJDocPageSink
// (exported from Go below) is that pointer; this thin wrapper lets Go invoke
// the stream with it while passing a cgo.Handle through userdata. The prototype
// must match cgo's generated one (non-const JDocPage*), so we cast to the
// JDocPageCallback signature (adding const to the pointee is safe) at the call.
extern int goJDocPageSink(JDocPage* page, void* userdata);
static int jdoc_stream_pages(const char* path, const JDocOptions* opts,
                             void* userdata, char* err, int errsz) {
    return jdoc_convert_pages_stream(path, opts,
        (JDocPageCallback)goJDocPageSink, userdata, err, errsz);
}
*/
import "C"

import (
	"errors"
	"iter"
	"runtime/cgo"
	"unsafe"
)

// Category is the coarse family a format belongs to.
type Category int

const (
	CategoryDocument Category = iota
	CategorySpreadsheet
	CategoryPresentation
	CategoryArchive
	CategoryEmail
	CategoryText
	CategoryImage
	CategoryUnknown
)

func (c Category) String() string {
	switch c {
	case CategoryDocument:
		return "document"
	case CategorySpreadsheet:
		return "spreadsheet"
	case CategoryPresentation:
		return "presentation"
	case CategoryArchive:
		return "archive"
	case CategoryEmail:
		return "email"
	case CategoryText:
		return "text"
	case CategoryImage:
		return "image"
	default:
		return "unknown"
	}
}

// FormatInfo is the rich result of Detect.
type FormatInfo struct {
	Format      string   // canonical name, e.g. "PDF", "DOCX", "PNG"
	Category    Category // coarse family
	Extension   string   // canonical extension incl. dot, e.g. ".pdf"
	MIME        string   // e.g. "application/pdf"; empty if unknown
	Convertible bool     // jdoc can extract text (Convert / ConvertArchive)
}

const errBufSize = 1024

// Detect identifies a file's format without running a full extraction.
func Detect(path string) (FormatInfo, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	var info C.JDocFormatInfo
	errBuf := make([]C.char, errBufSize)

	rc := C.jdoc_detect(cPath, &info, &errBuf[0], C.int(errBufSize))
	if rc != 0 {
		return FormatInfo{}, errors.New(C.GoString(&errBuf[0]))
	}
	defer C.jdoc_free_format_info(&info)
	return goFormatInfo(&info), nil
}

// DetectBytes identifies the format of an in-memory document. nameHint (may be
// "") resolves extension-based ambiguity.
func DetectBytes(data []byte, nameHint string) (FormatInfo, error) {
	if len(data) == 0 {
		return FormatInfo{}, errors.New("empty data")
	}
	cHint := C.CString(nameHint)
	defer C.free(unsafe.Pointer(cHint))

	var info C.JDocFormatInfo
	errBuf := make([]C.char, errBufSize)

	rc := C.jdoc_detect_mem(unsafe.Pointer(&data[0]), C.int(len(data)), cHint,
		&info, &errBuf[0], C.int(errBufSize))
	if rc != 0 {
		return FormatInfo{}, errors.New(C.GoString(&errBuf[0]))
	}
	defer C.jdoc_free_format_info(&info)
	return goFormatInfo(&info), nil
}

func goFormatInfo(info *C.JDocFormatInfo) FormatInfo {
	return FormatInfo{
		Format:      C.GoString(info.format),
		Category:    Category(info.category),
		Extension:   C.GoString(info.extension),
		MIME:        C.GoString(info.mime),
		Convertible: info.convertible != 0,
	}
}

// Options mirrors the C JDocOptions struct. Start from DefaultOptions and
// change what you need — the zero value is NOT a valid default (Tables would
// be off and MinImageSize 0 means "no size filter").
//
// The archive limit fields follow the C convention: 0 means "library default"
// and -1 disables that guard entirely. Only disable a guard for trusted input
// — archive-bomb protection goes with it.
type Options struct {
	Tables         bool   // render tables as markdown tables
	Images         bool   // extract images
	ImageDir       string // where to write images; "" keeps them in memory
	ImageRefPrefix string // prepended to image references in the markdown
	MinImageSize   uint   // skip images smaller than NxN (0 = no filter)
	Pages          []int  // page numbers to extract (nil = all)
	Format         string // "" or "markdown" (default), or "text"

	MaxDepth           int   // nested-archive depth (default 3)
	MaxMemberBytes     int64 // per-member uncompressed cap (default 512 MiB)
	MaxTotalBytes      int64 // cumulative cap per call (default 64 GiB)
	MaxEntries         int   // members visited (default 200000)
	MaxRatio           int   // bomb-suspect compression ratio (default 10000)
	IncludeUnsupported bool  // report unsupported members instead of skipping
}

// DefaultOptions returns the library defaults: tables on, no image extraction,
// markdown output, all pages, 50px minimum image size, standard archive limits.
func DefaultOptions() Options {
	return Options{Tables: true, MinImageSize: 50}
}

func cbool(b bool) C.int {
	if b {
		return 1
	}
	return 0
}

// toC builds a C JDocOptions in C memory (so cgo's pointer rules are satisfied:
// no Go pointers are stored in memory handed to C) and returns it with a
// cleanup func that frees it and every string/array it owns.
func (o Options) toC() (*C.JDocOptions, func()) {
	c := (*C.JDocOptions)(C.calloc(1, C.size_t(unsafe.Sizeof(C.JDocOptions{}))))
	var owned []unsafe.Pointer
	cstr := func(s string) *C.char {
		if s == "" {
			return nil
		}
		p := C.CString(s)
		owned = append(owned, unsafe.Pointer(p))
		return p
	}

	c.tables = cbool(o.Tables)
	c.images = cbool(o.Images)
	c.image_dir = cstr(o.ImageDir)
	c.image_ref_prefix = cstr(o.ImageRefPrefix)
	c.min_image_size = C.uint(o.MinImageSize)
	if n := len(o.Pages); n > 0 {
		arr := C.malloc(C.size_t(n) * C.size_t(unsafe.Sizeof(C.int(0))))
		owned = append(owned, arr)
		dst := unsafe.Slice((*C.int)(arr), n)
		for i, p := range o.Pages {
			dst[i] = C.int(p)
		}
		c.pages = (*C.int)(arr)
		c.page_count = C.int(n)
	}
	c.format = cstr(o.Format)
	c.max_depth = C.int(o.MaxDepth)
	c.max_member_bytes = C.longlong(o.MaxMemberBytes)
	c.max_total_bytes = C.longlong(o.MaxTotalBytes)
	c.max_entries = C.int(o.MaxEntries)
	c.max_ratio = C.int(o.MaxRatio)
	c.include_unsupported = cbool(o.IncludeUnsupported)

	return c, func() {
		for _, p := range owned {
			C.free(p)
		}
		C.free(unsafe.Pointer(c))
	}
}

// Convert converts a document to Markdown (or plain text) using default
// options. Returns an error for unsupported formats and archives (use
// ConvertArchive for the latter).
func Convert(path string) (string, error) { return convertWith(path, nil) }

// ConvertWithOptions is Convert with explicit options — notably Images and
// ImageDir to extract a document's images (and to convert a standalone image
// file, which is written to ImageDir as-is).
func ConvertWithOptions(path string, opts Options) (string, error) {
	c, free := opts.toC()
	defer free()
	return convertWith(path, c)
}

func convertWith(path string, opts *C.JDocOptions) (string, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	errBuf := make([]C.char, errBufSize)
	out := C.jdoc_convert(cPath, opts, &errBuf[0], C.int(errBufSize))
	if out == nil {
		return "", errors.New(C.GoString(&errBuf[0]))
	}
	defer C.jdoc_free_string(out)
	return C.GoString(out), nil
}

// Member is one document found inside an archive.
type Member struct {
	Path             string // e.g. "outer.zip/dir/report.hwp"
	Format           string
	Markdown         string
	Error            string
	ErrorCode        int
	UncompressedSize int64
}

// Ok reports whether the member converted successfully.
func (m Member) Ok() bool { return m.Error == "" }

// ConvertArchive converts every supported document inside an archive without
// extracting to disk, using default limits.
func ConvertArchive(path string) ([]Member, error) { return convertArchiveWith(path, nil) }

// ConvertArchiveWithOptions is ConvertArchive with explicit options — the
// archive limits, and Images/ImageDir to write out images found inside member
// documents as well as standalone image files stored in the archive. Extracted
// files mirror the archive's layout under ImageDir.
func ConvertArchiveWithOptions(path string, opts Options) ([]Member, error) {
	c, free := opts.toC()
	defer free()
	return convertArchiveWith(path, c)
}

func convertArchiveWith(path string, opts *C.JDocOptions) ([]Member, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	var count C.int
	errBuf := make([]C.char, errBufSize)
	arr := C.jdoc_convert_archive(cPath, opts, &count, &errBuf[0], C.int(errBufSize))
	if arr == nil {
		if msg := C.GoString(&errBuf[0]); msg != "" {
			return nil, errors.New(msg)
		}
		return nil, nil // no reportable members
	}
	defer C.jdoc_free_members(arr, count)

	n := int(count)
	members := make([]Member, n)
	slice := unsafe.Slice(arr, n)
	for i := 0; i < n; i++ {
		m := &slice[i]
		members[i] = Member{
			Path:             C.GoString(m.member_path),
			Format:           C.GoString(m.format),
			Markdown:         C.GoString(m.markdown),
			Error:            C.GoString(m.error),
			ErrorCode:        int(m.error_code),
			UncompressedSize: int64(m.uncompressed_size),
		}
	}
	return members, nil
}

// ── Pages ─────────────────────────────────────────────────────

// Image is one image extracted from a page.
type Image struct {
	PageNumber int
	Name       string
	Width      uint
	Height     uint
	Data       []byte // raw image bytes (jpeg/png/bmp)
	Format     string // "jpeg", "png", "bmp", ...
	SavedPath  string // disk path if image extraction wrote to a directory
}

// Page is one page/slide/sheet of a converted document.
type Page struct {
	PageNumber int
	Text       string // markdown or plaintext for this page
	Images     []Image
}

// goPage copies a borrowed C JDocPage into an owned Go Page.
func goPage(p *C.JDocPage) Page {
	page := Page{
		PageNumber: int(p.page_number),
		Text:       C.GoString(p.text),
	}
	n := int(p.image_count)
	if n > 0 && p.images != nil {
		imgs := unsafe.Slice(p.images, n)
		page.Images = make([]Image, n)
		for i := 0; i < n; i++ {
			si := &imgs[i]
			page.Images[i] = Image{
				PageNumber: int(si.page_number),
				Name:       C.GoString(si.name),
				Width:      uint(si.width),
				Height:     uint(si.height),
				Format:     C.GoString(si.format),
				SavedPath:  C.GoString(si.saved_path),
			}
			if si.data != nil && si.data_size > 0 {
				page.Images[i].Data = C.GoBytes(unsafe.Pointer(si.data), si.data_size)
			}
		}
	}
	return page
}

// ConvertPages converts a document to per-page chunks eagerly, using default
// options. For large documents prefer StreamPages, which yields one page at a
// time. Returns an error for unsupported formats and archives.
func ConvertPages(path string) ([]Page, error) { return convertPagesWith(path, nil) }

// ConvertPagesWithOptions is ConvertPages with explicit options — notably
// Images and ImageDir, which populate each page's Images (and write them to
// disk when ImageDir is set).
func ConvertPagesWithOptions(path string, opts Options) ([]Page, error) {
	c, free := opts.toC()
	defer free()
	return convertPagesWith(path, c)
}

func convertPagesWith(path string, opts *C.JDocOptions) ([]Page, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	var count C.int
	errBuf := make([]C.char, errBufSize)
	arr := C.jdoc_convert_pages(cPath, opts, &count, &errBuf[0], C.int(errBufSize))
	if arr == nil {
		if msg := C.GoString(&errBuf[0]); msg != "" {
			return nil, errors.New(msg)
		}
		return nil, nil // no pages
	}
	defer C.jdoc_free_pages(arr, count)

	n := int(count)
	pages := make([]Page, n)
	slice := unsafe.Slice(arr, n)
	for i := 0; i < n; i++ {
		pages[i] = goPage(&slice[i])
	}
	return pages, nil
}

// sinkState carries the caller's yield closure across the C boundary, plus any
// panic recovered while yielding (re-raised after the C call unwinds).
type sinkState struct {
	yield func(Page) bool
	panic any
}

//export goJDocPageSink
func goJDocPageSink(page *C.JDocPage, userdata unsafe.Pointer) C.int {
	s := cgo.Handle(userdata).Value().(*sinkState)
	// A panic must not cross the C frame; capture it, stop the stream, and let
	// StreamPages re-raise it once jdoc_stream_pages has returned.
	cont := false
	func() {
		defer func() {
			if r := recover(); r != nil {
				s.panic = r
			}
		}()
		cont = s.yield(goPage(page))
	}()
	if s.panic != nil || !cont {
		return 0 // stop the walk
	}
	return 1
}

// PageStream drives a lazy, single-pass walk over a document's pages. Iterate
// with Pages; after the loop, check Err for a conversion failure.
type PageStream struct {
	path string
	opts *Options // nil = C library defaults
	err  error
}

// StreamPages returns a lazy page iterator for the document at path, using
// default options. Pages are produced one at a time as they are consumed, so
// peak memory tracks a single page rather than the whole document, and the
// first page is available before the rest are parsed. Output matches
// ConvertPages.
func StreamPages(path string) *PageStream {
	return &PageStream{path: path}
}

// StreamPagesWithOptions is StreamPages with explicit options — notably Images
// and ImageDir, which populate each yielded page's Images.
func StreamPagesWithOptions(path string, opts Options) *PageStream {
	return &PageStream{path: path, opts: &opts}
}

// Pages returns a single-use iterator over the document's pages. Breaking out
// of the range loop stops the underlying conversion. Any conversion error is
// reported by Err after iteration ends.
func (s *PageStream) Pages() iter.Seq[Page] {
	return func(yield func(Page) bool) {
		state := &sinkState{yield: yield}
		h := cgo.NewHandle(state)
		defer h.Delete()

		cPath := C.CString(s.path)
		defer C.free(unsafe.Pointer(cPath))

		var cOpts *C.JDocOptions
		if s.opts != nil {
			c, free := s.opts.toC()
			defer free()
			cOpts = c
		}

		errBuf := make([]C.char, errBufSize)
		rc := C.jdoc_stream_pages(cPath, cOpts, unsafe.Pointer(h),
			&errBuf[0], C.int(errBufSize))

		if state.panic != nil {
			panic(state.panic) // re-raise a yield panic in the caller's goroutine
		}
		if rc != 0 {
			if msg := C.GoString(&errBuf[0]); msg != "" {
				s.err = errors.New(msg)
			}
		}
	}
}

// Err returns the error from the most recent Pages iteration, if any.
func (s *PageStream) Err() error { return s.err }
