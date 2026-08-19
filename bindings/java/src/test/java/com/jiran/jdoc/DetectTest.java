package com.jiran.jdoc;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayOutputStream;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

import static org.junit.jupiter.api.Assertions.*;

class DetectTest {

    // Repo root, three levels up from bindings/java.
    private static Path root() {
        return Paths.get(System.getProperty("user.dir")).resolve("../..").normalize();
    }

    @Test
    void defaultOptionsExtractImages() {
        assertTrue(new Options().images);
    }

    @Test
    void detectsPdfFile() {
        FormatInfo info = Jdoc.detect(
                root().resolve("test/fixtures/pdf/sample.pdf").toString());
        assertEquals("PDF", info.format);
        assertEquals(Category.DOCUMENT, info.category);
        assertEquals(".pdf", info.extension);
        assertTrue(info.convertible);
    }

    @Test
    void detectsArchiveFile() {
        FormatInfo info = Jdoc.detect(
                root().resolve("test/fixtures/7z/store.7z").toString());
        assertEquals("7Z", info.format);
        assertEquals(Category.ARCHIVE, info.category);
    }

    @Test
    void detectsImageFromBytes() throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        // PNG signature: 89 50 4E 47 0D 0A 1A 0A
        b.write(new byte[]{(byte) 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A});
        b.write(new byte[8]);
        FormatInfo info = Jdoc.detectBytes(b.toByteArray(), "");
        assertEquals("PNG", info.format);
        assertEquals(Category.IMAGE, info.category);
        assertFalse(info.convertible);
    }

    @Test
    void detectsTextFromBytes() {
        FormatInfo info = Jdoc.detectBytes(
                "hello world\n".getBytes(), "readme.txt");
        assertEquals("TXT", info.format);
        assertEquals(Category.TEXT, info.category);
    }

    @Test
    void convertsTextFromBytes() {
        assertEquals("hello world\n", Jdoc.convertBytes(
                "hello world\n".getBytes(), "readme.txt"));
    }

    @Test
    void convertsPagesFromBytesWithMetadata() throws Exception {
        byte[] data = Files.readAllBytes(
                root().resolve("test/fixtures/pdf/sample.pdf"));
        java.util.List<Page> pages = Jdoc.convertPagesBytes(data, "sample.pdf");
        assertFalse(pages.isEmpty());
        assertTrue(pages.get(0).pageWidth > 0);
        assertTrue(pages.get(0).pageHeight > 0);
    }

    @Test
    void emptyBytesAreAnEmptyTextDocument() {
        FormatInfo info = Jdoc.detectBytes(new byte[0], "empty.txt");
        assertEquals("TXT", info.format);
        assertEquals("", Jdoc.convertBytes(new byte[0], "empty.txt"));
        List<Page> pages = Jdoc.convertPagesBytes(new byte[0], "empty.txt");
        assertEquals(1, pages.size());
        assertEquals("", pages.get(0).text);
    }

    @Test
    void concurrentConversionsAreSafe() throws Exception {
        String path = root().resolve("test/fixtures/pdf/sample.pdf").toString();
        String expected = Jdoc.convert(path);
        ExecutorService pool = Executors.newFixedThreadPool(8);
        try {
            List<Callable<String>> calls = new ArrayList<>();
            for (int i = 0; i < 16; i++) calls.add(() -> Jdoc.convert(path));
            for (Future<String> result : pool.invokeAll(calls)) {
                assertEquals(expected, result.get());
            }
        } finally {
            pool.shutdownNow();
        }
    }

    @Test
    void invalidOptionsAreRejected() {
        String path = root().resolve("test/fixtures/pdf/sample.pdf").toString();
        assertThrows(IllegalArgumentException.class,
                () -> Jdoc.convert(path, new Options().pages(-1)));
        assertThrows(JdocException.class,
                () -> Jdoc.convert(path, new Options().format("html")));
    }
}
