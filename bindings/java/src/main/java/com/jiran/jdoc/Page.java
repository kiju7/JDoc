package com.jiran.jdoc;

import java.util.List;

/** One page/slide/sheet of a converted document. */
public final class Page {
    public final int pageNumber;
    public final String text;      // markdown or plaintext for this page
    public final List<Image> images;

    public Page(int pageNumber, String text, List<Image> images) {
        this.pageNumber = pageNumber;
        this.text = text;
        this.images = images;
    }
}
