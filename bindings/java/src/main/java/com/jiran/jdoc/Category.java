package com.jiran.jdoc;

/** Coarse family a detected format belongs to. Ordinals match the C
 *  {@code JDocFormatCategory} enum. */
public enum Category {
    DOCUMENT,
    SPREADSHEET,
    PRESENTATION,
    ARCHIVE,
    EMAIL,
    TEXT,
    IMAGE,
    UNKNOWN;

    private static final Category[] VALUES = values();

    /** Map a raw C category code to the enum (out-of-range → UNKNOWN). */
    public static Category fromCode(int code) {
        return (code >= 0 && code < VALUES.length) ? VALUES[code] : UNKNOWN;
    }
}
