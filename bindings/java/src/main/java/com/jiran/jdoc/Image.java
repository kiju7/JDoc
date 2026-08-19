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
    /** A format name, not a file extension: the file is written with the
     *  extension the source container declared, so "jpeg" lands as .jpg and a
     *  .webp part reports "bin". Never build a filename from it. */
    public final String format;
    /** The file that was written, when imageDir was set — the only correct
     *  source for the name on disk, collision suffix included. Empty when
     *  nothing was written, and the bytes are then in {@link #data}. */
    public final String savedPath;
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
