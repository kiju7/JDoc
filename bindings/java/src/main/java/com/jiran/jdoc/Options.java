package com.jiran.jdoc;

/**
 * Conversion options, mirroring the C {@code JDocOptions} struct.
 *
 * <p>The defaults here match {@code jdoc_default_options()}: tables on, no
 * images, markdown output, all pages, {@code minImageSize=50}. Archive limits
 * default to {@code 0}, which the library reads as "use the built-in default"
 * ({@code maxDepth=3}, {@code maxMemberBytes=512 MiB}, {@code maxTotalBytes=64
 * GiB}, {@code maxEntries=200000}, {@code maxRatio=10000}); {@code -1} disables
 * the corresponding guard — only for trusted inputs, since archive-bomb
 * protection goes with it.
 *
 * <p>Fields may be set directly or through the fluent setters:
 *
 * <pre>{@code
 * String md = Jdoc.convert("report.pdf",
 *         new Options().images(true).imageDir("/tmp/img"));
 * }</pre>
 */
public final class Options {

    /** Render tables as markdown tables. */
    public boolean tables = true;
    /** Extract images embedded in the document. */
    public boolean images = false;
    /** Image output directory; null keeps images in memory. */
    public String imageDir = null;
    /** Prepended to image references in the markdown; null = none. */
    public String imageRefPrefix = null;
    /** Skip images smaller than NxN pixels; 0 = no filter. */
    public int minImageSize = 50;
    /** Page numbers to extract (1-based); null = all pages. */
    public int[] pages = null;
    /** "markdown" (default) or "text". */
    public String format = "markdown";
    /** Archive nesting depth; 0 = default (3), -1 = unlimited. */
    public int maxDepth = 0;
    /** Per-member uncompressed cap; 0 = default (512 MiB), -1 = unlimited. */
    public long maxMemberBytes = 0;
    /** Cumulative uncompressed cap per call; 0 = default (64 GiB), -1 = unlimited. */
    public long maxTotalBytes = 0;
    /** Archive members visited; 0 = default (200000), -1 = unlimited. */
    public int maxEntries = 0;
    /** Bomb-suspect compression ratio; 0 = default (10000), -1 = check off. */
    public int maxRatio = 0;
    /** Report archive members jdoc cannot convert. */
    public boolean includeUnsupported = false;

    public Options() {}

    // ── fluent setters ────────────────────────────────────────

    public Options tables(boolean v) { this.tables = v; return this; }

    public Options images(boolean v) { this.images = v; return this; }

    public Options imageDir(String v) { this.imageDir = v; return this; }

    public Options imageRefPrefix(String v) { this.imageRefPrefix = v; return this; }

    public Options minImageSize(int v) { this.minImageSize = v; return this; }

    public Options pages(int... v) { this.pages = v; return this; }

    public Options format(String v) { this.format = v; return this; }

    public Options maxDepth(int v) { this.maxDepth = v; return this; }

    public Options maxMemberBytes(long v) { this.maxMemberBytes = v; return this; }

    public Options maxTotalBytes(long v) { this.maxTotalBytes = v; return this; }

    public Options maxEntries(int v) { this.maxEntries = v; return this; }

    public Options maxRatio(int v) { this.maxRatio = v; return this; }

    public Options includeUnsupported(boolean v) { this.includeUnsupported = v; return this; }

    // ── native marshalling ────────────────────────────────────

    /** Build the native option struct. The returned Structure owns the native
     *  buffers behind its pointer fields, so it must stay reachable for the
     *  whole native call. Returns null for null options (C defaults). */
    static JdocLibrary.JDocOptions toNative(Options opts) {
        if (opts == null) return null;

        JdocLibrary.JDocOptions o = new JdocLibrary.JDocOptions();
        o.tables = opts.tables ? 1 : 0;
        o.images = opts.images ? 1 : 0;
        o.image_dir = o.retain(opts.imageDir);
        o.image_ref_prefix = o.retain(opts.imageRefPrefix);
        o.min_image_size = opts.minImageSize;
        o.pages = o.retain(opts.pages);
        o.page_count = (opts.pages == null) ? 0 : opts.pages.length;
        o.format = o.retain(opts.format);
        o.max_depth = opts.maxDepth;
        o.max_member_bytes = opts.maxMemberBytes;
        o.max_total_bytes = opts.maxTotalBytes;
        o.max_entries = opts.maxEntries;
        o.max_ratio = opts.maxRatio;
        o.include_unsupported = opts.includeUnsupported ? 1 : 0;
        return o;
    }
}
