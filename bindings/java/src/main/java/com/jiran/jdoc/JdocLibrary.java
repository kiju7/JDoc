package com.jiran.jdoc;

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
}
