package com.jiran.jdoc;

/** One image extracted from a page. */
public final class Image {
    public final int pageNumber;
    public final String name;
    public final int width;
    public final int height;
    public final int components;
    public final byte[] data;     // encoded image bytes (jpeg/png/bmp); may be empty
    public final byte[] pixels;   // raw pixels; may be empty
    public final String format;   // "jpeg", "png", "bmp", ...
    public final String savedPath; // disk path if image extraction wrote to a directory
    public final String embeddedText; // text recovered from EMF/WMF

    public Image(int pageNumber, String name, int width, int height,
                 byte[] data, String format, String savedPath) {
        this(pageNumber, name, width, height, 0, data, new byte[0], format,
                savedPath, "");
    }

    public Image(int pageNumber, String name, int width, int height,
                 int components, byte[] data, byte[] pixels, String format,
                 String savedPath, String embeddedText) {
        this.pageNumber = pageNumber;
        this.name = name;
        this.width = width;
        this.height = height;
        this.components = components;
        this.data = data;
        this.pixels = pixels;
        this.format = format;
        this.savedPath = savedPath;
        this.embeddedText = embeddedText;
    }
}
