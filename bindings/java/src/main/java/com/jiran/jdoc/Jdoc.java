package com.jiran.jdoc;

import com.sun.jna.Pointer;

import java.nio.charset.StandardCharsets;

/**
 * High-level Java API for the JDoc document converter.
 *
 * <pre>{@code
 * FormatInfo info = Jdoc.detect("report.pdf");
 * // info.format="PDF", info.category=Category.DOCUMENT, info.convertible=true
 *
 * String markdown = Jdoc.convert("report.pdf");
 * }</pre>
 *
 * <p>Requires the native {@code libjdoc} on {@code -Djna.library.path}.
 */
public final class Jdoc {

    private static final int ERR_BUF_SIZE = 1024;

    private Jdoc() {}

    /** Detect a file's format without running a full extraction. */
    public static FormatInfo detect(String filePath) {
        JdocLibrary lib = JdocLibrary.INSTANCE;
        JdocLibrary.JDocFormatInfo out = new JdocLibrary.JDocFormatInfo();
        byte[] err = new byte[ERR_BUF_SIZE];

        int rc = lib.jdoc_detect(filePath, out, err, ERR_BUF_SIZE);
        if (rc != 0) {
            throw new JdocException(cString(err));
        }
        try {
            return toFormatInfo(out);
        } finally {
            lib.jdoc_free_format_info(out);
        }
    }

    /** Detect the format of an in-memory document. nameHint (may be "" or null)
     *  resolves extension-based ambiguity. */
    public static FormatInfo detectBytes(byte[] data, String nameHint) {
        if (data == null || data.length == 0) {
            throw new JdocException("empty data");
        }
        JdocLibrary lib = JdocLibrary.INSTANCE;
        JdocLibrary.JDocFormatInfo out = new JdocLibrary.JDocFormatInfo();
        byte[] err = new byte[ERR_BUF_SIZE];

        int rc = lib.jdoc_detect_mem(data, data.length,
                nameHint == null ? "" : nameHint, out, err, ERR_BUF_SIZE);
        if (rc != 0) {
            throw new JdocException(cString(err));
        }
        try {
            return toFormatInfo(out);
        } finally {
            lib.jdoc_free_format_info(out);
        }
    }

    /** Convert a document to Markdown using default options. Throws for
     *  unsupported formats and archives. */
    public static String convert(String filePath) {
        JdocLibrary lib = JdocLibrary.INSTANCE;
        byte[] err = new byte[ERR_BUF_SIZE];
        Pointer p = lib.jdoc_convert(filePath, null, err, ERR_BUF_SIZE);
        if (p == null) {
            throw new JdocException(cString(err));
        }
        try {
            return p.getString(0, "UTF-8");
        } finally {
            lib.jdoc_free_string(p);
        }
    }

    private static FormatInfo toFormatInfo(JdocLibrary.JDocFormatInfo out) {
        return new FormatInfo(
                str(out.format),
                Category.fromCode(out.category),
                str(out.extension),
                str(out.mime),
                out.convertible != 0);
    }

    private static String str(Pointer p) {
        return p == null ? "" : p.getString(0, "UTF-8");
    }

    /** Read a NUL-terminated UTF-8 C string out of a fixed byte buffer. */
    private static String cString(byte[] buf) {
        int len = 0;
        while (len < buf.length && buf[len] != 0) len++;
        return new String(buf, 0, len, StandardCharsets.UTF_8);
    }
}
