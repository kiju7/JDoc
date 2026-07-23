package com.jiran.jdoc;

import org.junit.jupiter.api.Test;

import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class PageStreamTest {

    // Repo root, three levels up from bindings/java.
    private static Path root() {
        return Paths.get(System.getProperty("user.dir")).resolve("../..").normalize();
    }

    private static String samplePdf() {
        return root().resolve("test/fixtures/pdf/sample.pdf").toString();
    }

    @Test
    void streamMatchesEager() {
        String path = samplePdf();
        List<Page> eager = Jdoc.convertPages(path);

        List<Page> streamed = new ArrayList<>();
        try (PageStream ps = Jdoc.streamPages(path)) {
            for (Page p : ps) streamed.add(p);
        }

        assertEquals(eager.size(), streamed.size());
        for (int i = 0; i < eager.size(); i++) {
            assertEquals(eager.get(i).pageNumber, streamed.get(i).pageNumber);
            assertEquals(eager.get(i).text, streamed.get(i).text);
        }
    }

    @Test
    void earlyBreakStops() {
        int seen = 0;
        try (PageStream ps = Jdoc.streamPages(samplePdf())) {
            for (Page p : ps) {
                seen++;
                break;
            }
        }
        assertEquals(1, seen);
    }

    @Test
    void missingFileThrows() {
        assertThrows(JdocException.class, () -> {
            try (PageStream ps = Jdoc.streamPages(
                    root().resolve("does-not-exist.pdf").toString())) {
                for (Page p : ps) {
                    // drain
                }
            }
        });
    }
}
