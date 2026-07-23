package com.jiran.jdoc;

import org.junit.jupiter.api.Test;

import java.io.ByteArrayOutputStream;
import java.nio.file.Path;
import java.nio.file.Paths;

import static org.junit.jupiter.api.Assertions.*;

class DetectTest {

    // Repo root, three levels up from bindings/java.
    private static Path root() {
        return Paths.get(System.getProperty("user.dir")).resolve("../..").normalize();
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
}
