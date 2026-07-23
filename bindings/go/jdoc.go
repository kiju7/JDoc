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
#cgo CFLAGS: -I${SRCDIR}/../../include
#cgo LDFLAGS: -L${SRCDIR}/../../build -ljdoc
#cgo linux LDFLAGS: -Wl,-rpath,${SRCDIR}/../../build
#cgo darwin LDFLAGS: -Wl,-rpath,${SRCDIR}/../../build
#include <stdlib.h>
#include <jdoc/jdoc_c_api.h>
*/
import "C"

import (
	"errors"
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

// Convert converts a document to Markdown (or plain text) using default
// options. Returns an error for unsupported formats and archives (use
// ConvertArchive for the latter).
func Convert(path string) (string, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	errBuf := make([]C.char, errBufSize)
	out := C.jdoc_convert(cPath, nil, &errBuf[0], C.int(errBufSize))
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
func ConvertArchive(path string) ([]Member, error) {
	cPath := C.CString(path)
	defer C.free(unsafe.Pointer(cPath))

	var count C.int
	errBuf := make([]C.char, errBufSize)
	arr := C.jdoc_convert_archive(cPath, nil, &count, &errBuf[0], C.int(errBufSize))
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
