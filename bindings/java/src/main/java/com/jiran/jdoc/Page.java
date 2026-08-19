package com.jiran.jdoc;

import java.util.List;

/** One page/slide/sheet of a converted document. */
public final class Page {
    public final int pageNumber;
    public final String text;      // markdown or plaintext for this page
    public final double pageWidth;
    public final double pageHeight;
    public final double bodyFontSize;
    public final List<List<List<String>>> tables;
    public final List<Image> images;
    public final int degradedImages;

    public Page(int pageNumber, String text, List<Image> images) {
        this(pageNumber, text, 0, 0, 0, java.util.Collections.emptyList(),
                images, 0);
    }

    public Page(int pageNumber, String text, double pageWidth,
                double pageHeight, double bodyFontSize,
                List<List<List<String>>> tables, List<Image> images,
                int degradedImages) {
        this.pageNumber = pageNumber;
        this.text = text;
        this.pageWidth = pageWidth;
        this.pageHeight = pageHeight;
        this.bodyFontSize = bodyFontSize;
        this.tables = tables;
        this.images = images;
        this.degradedImages = degradedImages;
    }
}
