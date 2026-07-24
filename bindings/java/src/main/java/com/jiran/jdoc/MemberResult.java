package com.jiran.jdoc;

/** One document found inside an archive by {@link Jdoc#convertArchive}. */
public final class MemberResult {
    public final String memberPath;    // e.g. "outer.zip/dir/report.hwp"
    public final String format;        // "PDF", "HWP", "ZIP", ...
    public final String markdown;      // empty when the member failed
    public final String error;         // empty when the member converted
    public final int errorCode;        // C JDocMemberErrorCode; 0 = OK
    public final long uncompressedSize;

    public MemberResult(String memberPath, String format, String markdown,
                        String error, int errorCode, long uncompressedSize) {
        this.memberPath = memberPath;
        this.format = format;
        this.markdown = markdown;
        this.error = error;
        this.errorCode = errorCode;
        this.uncompressedSize = uncompressedSize;
    }

    /** True if the member converted successfully. */
    public boolean ok() {
        return error == null || error.isEmpty();
    }
}
