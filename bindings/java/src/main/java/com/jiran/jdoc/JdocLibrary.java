package com.jiran.jdoc;

import com.sun.jna.Callback;
import com.sun.jna.Library;
import com.sun.jna.Memory;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.Structure;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Low-level JNA mapping of the JDoc C API (include/jdoc/jdoc_c_api.h).
 *
 * <p>The native library {@code libjdoc} (libjdoc.dylib / .so / jdoc.dll) must be
 * discoverable at runtime — set {@code -Djna.library.path=<dir with libjdoc>}
 * or install it to a standard location.
 *
 * <p>Most callers should use the friendly {@link Jdoc} wrapper instead of this
 * interface directly.
 */
public interface JdocLibrary extends Library {

    JdocLibrary INSTANCE = Native.load("jdoc", JdocLibrary.class);

    /** Mirrors the C {@code JDocOptions} struct. A zero-filled struct is NOT a
     *  valid default (tables would be off and images unfiltered) — build one
     *  from {@link Options} instead, or pass null for the C defaults. */
    @Structure.FieldOrder({"tables", "images", "image_dir", "image_ref_prefix",
                           "min_image_size", "pages", "page_count", "format",
                           "max_depth", "max_member_bytes", "max_total_bytes",
                           "max_entries", "max_ratio", "include_unsupported"})
    class JDocOptions extends Structure {
        public int tables;
        public int images;
        public Pointer image_dir;          // const char*
        public Pointer image_ref_prefix;   // const char*
        public int min_image_size;         // unsigned int
        public Pointer pages;              // const int[page_count]
        public int page_count;
        public Pointer format;             // const char*
        public int max_depth;
        public long max_member_bytes;      // long long
        public long max_total_bytes;       // long long
        public int max_entries;
        public int max_ratio;
        public int include_unsupported;

        // Native buffers behind the pointer fields above. Held here so they
        // stay alive as long as the struct does (JNA frees Memory once it
        // becomes unreachable). Non-public, so JNA ignores it as a field.
        private final List<Memory> retained = new ArrayList<>();

        public JDocOptions() {}
        public JDocOptions(Pointer p) { super(p); }

        /** Copy a Java string into native UTF-8 memory owned by this struct. */
        Pointer retain(String s) {
            if (s == null) return null;
            byte[] utf8 = s.getBytes(StandardCharsets.UTF_8);
            Memory m = new Memory(utf8.length + 1);
            m.write(0, utf8, 0, utf8.length);
            m.setByte(utf8.length, (byte) 0);
            retained.add(m);
            return m;
        }

        /** Copy a page-number array into native memory owned by this struct. */
        Pointer retain(int[] values) {
            if (values == null || values.length == 0) return null;
            Memory m = new Memory(4L * values.length);
            m.write(0, values, 0, values.length);
            retained.add(m);
            return m;
        }

        /** By-value variant, for {@link #jdoc_default_options()}. */
        public static class ByValue extends JDocOptions implements Structure.ByValue {}
    }

    /** Returns the library defaults (no images, markdown, all pages,
     *  min_image_size 50). {@link Options} mirrors these in pure Java. */
    JDocOptions.ByValue jdoc_default_options();

    /** Mirrors the C {@code JDocFormatInfo} struct. Strings are owned by the
     *  native side; copy them out before calling {@link #jdoc_free_format_info}. */
    @Structure.FieldOrder({"format", "category", "extension", "mime", "convertible"})
    class JDocFormatInfo extends Structure {
        public Pointer format;
        public int category;
        public Pointer extension;
        public Pointer mime;
        public int convertible;

        public static class ByReference extends JDocFormatInfo
                implements Structure.ByReference {}
    }

    int jdoc_detect(String filePath, JDocFormatInfo out,
                    byte[] errBuf, int errBufSize);

    int jdoc_detect_mem(byte[] data, int size, String nameHint,
                        JDocFormatInfo out, byte[] errBuf, int errBufSize);

    void jdoc_free_format_info(JDocFormatInfo info);

    Pointer jdoc_convert(String filePath, Pointer opts,
                         byte[] errBuf, int errBufSize);

    /** Same native function, taking a filled-in option struct (null = defaults). */
    Pointer jdoc_convert(String filePath, JDocOptions opts,
                         byte[] errBuf, int errBufSize);

    void jdoc_free_string(Pointer str);

    /** Mirrors the C {@code JDocImage} struct. All pointers are owned natively. */
    @Structure.FieldOrder({"page_number", "name", "width", "height",
                           "data", "data_size", "format", "saved_path"})
    class JDocImage extends Structure {
        public int page_number;
        public Pointer name;
        public int width;
        public int height;
        public Pointer data;
        public int data_size;
        public Pointer format;
        public Pointer saved_path;

        public JDocImage() {}
        public JDocImage(Pointer p) { super(p); }
    }

    /** Mirrors the C {@code JDocPage} struct. */
    @Structure.FieldOrder({"page_number", "text", "images", "image_count"})
    class JDocPage extends Structure {
        public int page_number;
        public Pointer text;
        public Pointer images;      // JDocImage[image_count]
        public int image_count;

        public JDocPage() {}
        public JDocPage(Pointer p) { super(p); }
    }

    /** Mirrors the C {@code JDocPageCallback}: return nonzero to continue, 0 to
     *  stop. {@code page} is borrowed for the duration of the call. Keep a
     *  strong reference to any instance passed to native code (JNA GC pitfall). */
    interface JDocPageCallback extends Callback {
        int invoke(Pointer page, Pointer userdata);
    }

    Pointer jdoc_convert_pages(String filePath, Pointer opts,
                               com.sun.jna.ptr.IntByReference outCount,
                               byte[] errBuf, int errBufSize);

    int jdoc_convert_pages_stream(String filePath, Pointer opts,
                                  JDocPageCallback cb, Pointer userdata,
                                  byte[] errBuf, int errBufSize);

    /** Same native functions, taking a filled-in option struct (null = defaults). */
    Pointer jdoc_convert_pages(String filePath, JDocOptions opts,
                               com.sun.jna.ptr.IntByReference outCount,
                               byte[] errBuf, int errBufSize);

    int jdoc_convert_pages_stream(String filePath, JDocOptions opts,
                                  JDocPageCallback cb, Pointer userdata,
                                  byte[] errBuf, int errBufSize);

    void jdoc_free_pages(Pointer pages, int count);

    /** Mirrors the C {@code JDocMember} struct. Strings are owned by the
     *  native side; copy them out before calling {@link #jdoc_free_members}. */
    @Structure.FieldOrder({"member_path", "format", "markdown", "error",
                           "error_code", "uncompressed_size"})
    class JDocMember extends Structure {
        public Pointer member_path;
        public Pointer format;
        public Pointer markdown;     // NULL on error
        public Pointer error;        // NULL on success
        public int error_code;       // JDocMemberErrorCode; 0 = OK
        public long uncompressed_size;

        public JDocMember() {}
        public JDocMember(Pointer p) { super(p); }
    }

    Pointer jdoc_convert_archive(String filePath, JDocOptions opts,
                                 com.sun.jna.ptr.IntByReference outCount,
                                 byte[] errBuf, int errBufSize);

    void jdoc_free_members(Pointer members, int count);
}
