package com.jiran.jdoc;

import com.sun.jna.Pointer;
import com.sun.jna.ptr.IntByReference;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

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
        return convert(filePath, null);
    }

    /** Convert a document to Markdown. {@code opts} may be null for defaults;
     *  set {@link Options#images}/{@link Options#imageDir} to extract images.
     *  Throws for unsupported formats and archives. */
    public static String convert(String filePath, Options opts) {
        JdocLibrary lib = JdocLibrary.INSTANCE;
        JdocLibrary.JDocOptions nativeOpts = Options.toNative(opts);
        byte[] err = new byte[ERR_BUF_SIZE];
        Pointer p = lib.jdoc_convert(filePath, nativeOpts, err, ERR_BUF_SIZE);
        if (p == null) {
            throw new JdocException(cString(err));
        }
        try {
            return p.getString(0, "UTF-8");
        } finally {
            lib.jdoc_free_string(p);
        }
    }

    /** Convert a document to per-page chunks eagerly, using default options.
     *  For large documents prefer {@link #streamPages}, which yields one page
     *  at a time. Throws for unsupported formats and archives. */
    public static List<Page> convertPages(String filePath) {
        return convertPages(filePath, null);
    }

    /** Convert a document to per-page chunks eagerly. {@code opts} may be null
     *  for defaults. For large documents prefer {@link #streamPages}, which
     *  yields one page at a time. Throws for unsupported formats and archives. */
    public static List<Page> convertPages(String filePath, Options opts) {
        JdocLibrary lib = JdocLibrary.INSTANCE;
        JdocLibrary.JDocOptions nativeOpts = Options.toNative(opts);
        byte[] err = new byte[ERR_BUF_SIZE];
        IntByReference count = new IntByReference();
        Pointer arr = lib.jdoc_convert_pages(filePath, nativeOpts, count, err, ERR_BUF_SIZE);
        if (arr == null) {
            String msg = cString(err);
            if (!msg.isEmpty()) throw new JdocException(msg);
            return new ArrayList<>();  // no pages
        }
        try {
            int n = count.getValue();
            List<Page> pages = new ArrayList<>(n);
            if (n > 0) {
                JdocLibrary.JDocPage first = new JdocLibrary.JDocPage(arr);
                first.read();
                JdocLibrary.JDocPage[] native_pages =
                        (JdocLibrary.JDocPage[]) first.toArray(n);
                for (JdocLibrary.JDocPage np : native_pages) {
                    pages.add(PageStream.fromNative(np));
                }
            }
            return pages;
        } finally {
            lib.jdoc_free_pages(arr, count.getValue());
        }
    }

    /** Open a lazy page iterator for a document, using default options. Yields
     *  one page at a time so peak memory tracks a few pages, not the whole
     *  document. The result must be closed (try-with-resources); see
     *  {@link PageStream}. Output matches {@link #convertPages}. */
    public static PageStream streamPages(String filePath) {
        return streamPages(filePath, null);
    }

    /** Open a lazy page iterator for a document. {@code opts} may be null for
     *  defaults. The result must be closed (try-with-resources); see
     *  {@link PageStream}. Output matches {@link #convertPages}. */
    public static PageStream streamPages(String filePath, Options opts) {
        return new PageStream(filePath, opts, 4);
    }

    /** Convert every supported document inside an archive without extracting to
     *  disk, using default options and limits. */
    public static List<MemberResult> convertArchive(String filePath) {
        return convertArchive(filePath, null);
    }

    /** Convert every supported document inside an archive (zip/gz/tar/tar.gz/
     *  7z/alz/egg) without extracting to disk. Per-member failures are reported
     *  in that member's {@link MemberResult#error}, not thrown; only a file that
     *  cannot be opened at all raises. {@code opts} may be null for defaults —
     *  set {@link Options#images}/{@link Options#imageDir} to save images, and
     *  the {@code max*} fields to adjust the archive-bomb guards. */
    public static List<MemberResult> convertArchive(String filePath, Options opts) {
        JdocLibrary lib = JdocLibrary.INSTANCE;
        JdocLibrary.JDocOptions nativeOpts = Options.toNative(opts);
        byte[] err = new byte[ERR_BUF_SIZE];
        IntByReference count = new IntByReference();
        Pointer arr = lib.jdoc_convert_archive(filePath, nativeOpts, count, err, ERR_BUF_SIZE);
        if (arr == null) {
            String msg = cString(err);
            if (!msg.isEmpty()) throw new JdocException(msg);
            return new ArrayList<>();  // no reportable members
        }
        try {
            int n = count.getValue();
            List<MemberResult> members = new ArrayList<>(n);
            if (n > 0) {
                JdocLibrary.JDocMember first = new JdocLibrary.JDocMember(arr);
                first.read();
                JdocLibrary.JDocMember[] native_members =
                        (JdocLibrary.JDocMember[]) first.toArray(n);
                for (JdocLibrary.JDocMember nm : native_members) {
                    members.add(new MemberResult(
                            str(nm.member_path),
                            str(nm.format),
                            str(nm.markdown),
                            str(nm.error),
                            nm.error_code,
                            nm.uncompressed_size));
                }
            }
            return members;
        } finally {
            lib.jdoc_free_members(arr, count.getValue());
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
