package com.jiran.jdoc;

/** Rich result of {@link Jdoc#detect}. */
public final class FormatInfo {
    /** Canonical name, e.g. "PDF", "DOCX", "PNG", "UNKNOWN". */
    public final String format;
    public final Category category;
    /** Canonical extension incl. dot, e.g. ".pdf". */
    public final String extension;
    /** e.g. "application/pdf"; empty if unknown. */
    public final String mime;
    /** True if jdoc can extract text (convert / convert_archive). */
    public final boolean convertible;

    public FormatInfo(String format, Category category, String extension,
                      String mime, boolean convertible) {
        this.format = format;
        this.category = category;
        this.extension = extension;
        this.mime = mime;
        this.convertible = convertible;
    }

    @Override
    public String toString() {
        return "FormatInfo{format='" + format + "', category=" + category
                + ", extension='" + extension + "', mime='" + mime
                + "', convertible=" + convertible + '}';
    }
}
