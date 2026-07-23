package com.jiran.jdoc;

/** One image extracted from a page. */
public final class Image {
    public final int pageNumber;
    public final String name;
    public final int width;
    public final int height;
    public final byte[] data;     // raw image bytes (jpeg/png/bmp); may be empty
    public final String format;   // "jpeg", "png", "bmp", ...
    public final String savedPath; // disk path if image extraction wrote to a directory

    public Image(int pageNumber, String name, int width, int height,
                 byte[] data, String format, String savedPath) {
        this.pageNumber = pageNumber;
        this.name = name;
        this.width = width;
        this.height = height;
        this.data = data;
        this.format = format;
        this.savedPath = savedPath;
    }
}
