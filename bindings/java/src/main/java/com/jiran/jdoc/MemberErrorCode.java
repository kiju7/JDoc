package com.jiran.jdoc;

/** Machine-readable archive member failure classification. */
public enum MemberErrorCode {
    OK,
    MEMBER_LIMIT,
    RATIO_LIMIT,
    TOTAL_LIMIT,
    ENTRY_LIMIT,
    DEPTH_LIMIT,
    ENCRYPTED,
    UNSUPPORTED,
    CORRUPT,
    CONVERT_FAILED;

    private static final MemberErrorCode[] VALUES = values();

    public static MemberErrorCode fromCode(int code) {
        return (code >= 0 && code < VALUES.length) ? VALUES[code] : CONVERT_FAILED;
    }
}
