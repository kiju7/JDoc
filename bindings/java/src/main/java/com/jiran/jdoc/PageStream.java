package com.jiran.jdoc;

import com.sun.jna.Pointer;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.NoSuchElementException;
import java.util.Spliterator;
import java.util.Spliterators;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.BlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.stream.Stream;
import java.util.stream.StreamSupport;

/**
 * Lazy, single-pass iterator over a document's pages, backed by the native
 * streaming API ({@code jdoc_convert_pages_stream}).
 *
 * <p>A background thread runs the conversion and hands each page to a bounded
 * queue; the consumer pulls from it. The bound caps producer/consumer skew so
 * peak memory tracks a few pages rather than the whole document, and the first
 * page is available before the rest are parsed. Output matches
 * {@link Jdoc#convertPages}.
 *
 * <p><b>Always close the stream</b> (try-with-resources) — closing stops the
 * background conversion. Iterating to completion also stops it.
 *
 * <pre>{@code
 * try (PageStream ps = Jdoc.streamPages("report.pdf")) {
 *     for (Page page : ps) {
 *         System.out.println(page.text);
 *     }
 * }
 * }</pre>
 */
public final class PageStream implements Iterable<Page>, AutoCloseable {

    private static final int ERR_BUF_SIZE = 1024;
    private static final Object DONE = new Object();

    private final BlockingQueue<Object> queue;
    private final Thread producer;
    // Strong reference: JNA callbacks may be garbage-collected while native code
    // still holds them, so we must keep this alive for the stream's lifetime.
    private final JdocLibrary.JDocPageCallback callback;
    private volatile boolean stop = false;
    private volatile String error = null;

    PageStream(String filePath, int capacity) {
        this.queue = new ArrayBlockingQueue<>(Math.max(1, capacity));

        this.callback = (pagePtr, userdata) -> {
            if (stop) return 0;
            Page page = toPage(pagePtr);
            // Block until the consumer makes room, but stay responsive to close().
            while (!stop) {
                try {
                    if (queue.offer(page, 100, TimeUnit.MILLISECONDS)) break;
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    return 0;
                }
            }
            return stop ? 0 : 1;
        };

        this.producer = new Thread(() -> {
            byte[] err = new byte[ERR_BUF_SIZE];
            int rc = JdocLibrary.INSTANCE.jdoc_convert_pages_stream(
                    filePath, null, callback, null, err, ERR_BUF_SIZE);
            if (rc != 0) {
                String msg = cString(err);
                if (!msg.isEmpty()) error = msg;
            }
            offerUntilStop(DONE);
        }, "jdoc-page-stream");
        this.producer.setDaemon(true);
        this.producer.start();
    }

    private void offerUntilStop(Object item) {
        while (!stop) {
            try {
                if (queue.offer(item, 100, TimeUnit.MILLISECONDS)) return;
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    @Override
    public Iterator<Page> iterator() {
        return new Iterator<Page>() {
            private Object lookahead = null;
            private boolean finished = false;

            @Override
            public boolean hasNext() {
                if (finished) return false;
                if (lookahead != null) return true;
                Object o;
                try {
                    o = queue.take();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    finished = true;
                    return false;
                }
                if (o == DONE) {
                    finished = true;
                    if (error != null) throw new JdocException(error);
                    return false;
                }
                lookahead = o;
                return true;
            }

            @Override
            public Page next() {
                if (!hasNext()) throw new NoSuchElementException();
                Page p = (Page) lookahead;
                lookahead = null;
                return p;
            }
        };
    }

    /** A sequential {@link Stream} over the pages. Closing the stream closes
     *  this PageStream (stopping the background conversion). */
    public Stream<Page> stream() {
        Spliterator<Page> sp = Spliterators.spliteratorUnknownSize(
                iterator(), Spliterator.ORDERED | Spliterator.NONNULL);
        return StreamSupport.stream(sp, false).onClose(this::close);
    }

    @Override
    public void close() {
        stop = true;
        producer.interrupt();
        queue.clear();  // unblock a producer stuck offering into a full queue
        try {
            producer.join(2000);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    // ── native → Java marshalling ─────────────────────────────

    private static Page toPage(Pointer pagePtr) {
        JdocLibrary.JDocPage np = new JdocLibrary.JDocPage(pagePtr);
        np.read();
        return fromNative(np);
    }

    /** Convert an already-read native JDocPage struct into a Java Page. Shared
     *  with {@link Jdoc#convertPages} (eager path). */
    static Page fromNative(JdocLibrary.JDocPage np) {
        List<Image> images = new ArrayList<>();
        if (np.image_count > 0 && np.images != null) {
            JdocLibrary.JDocImage first = new JdocLibrary.JDocImage(np.images);
            first.read();
            JdocLibrary.JDocImage[] arr =
                    (JdocLibrary.JDocImage[]) first.toArray(np.image_count);
            for (JdocLibrary.JDocImage si : arr) {
                byte[] data = (si.data != null && si.data_size > 0)
                        ? si.data.getByteArray(0, si.data_size) : new byte[0];
                images.add(new Image(si.page_number, str(si.name),
                        si.width, si.height, data, str(si.format), str(si.saved_path)));
            }
        }
        return new Page(np.page_number, str(np.text), images);
    }

    static String str(Pointer p) {
        return p == null ? "" : p.getString(0, "UTF-8");
    }

    private static String cString(byte[] buf) {
        int len = 0;
        while (len < buf.length && buf[len] != 0) len++;
        return new String(buf, 0, len, StandardCharsets.UTF_8);
    }
}
