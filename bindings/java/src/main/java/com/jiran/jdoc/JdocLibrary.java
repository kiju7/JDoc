package com.jiran.jdoc;

import com.sun.jna.Callback;
import com.sun.jna.Library;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.Structure;

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

    void jdoc_free_pages(Pointer pages, int count);
}
