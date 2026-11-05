package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;
import java.util.zip.CRC32;

/**
 * Finds out which input files the installed game build reads (design section 3.3). Pure Java.
 *
 * A game build contains the name of every file it reads as a zero-terminated string. The scan looks for the names of
 * the runtime files of RuntimeMapping (and five markers) in files/private-trace/private-trace-live.so, the library the
 * game loads. The result is cached in files/settings/capabilities.txt and reused while the library's size and
 * modification time stay the same.
 */
final class CapabilityScanner {
    private CapabilityScanner() { }

    /** The game library, relative to files/. Only read. */
    static final String LIBRARY = "private-trace/private-trace-live.so";
    /** Optional file placed by the lead, relative to files/: lines "+name" (count as found) and "-name" (count as missing). Only read. */
    static final String OVERRIDE = "settings/capabilities-override.txt";

    private static final int BUFFER = 1 << 20;

    /**
     * What the scan looks for: the base names of the files a build may read for a setting (the two frame-generation
     * switches diag-fg-between and diag-fg-ahead among them), the name of the GSR 1 developer switch (only looked at,
     * see RuntimeMapping.GSR_SWITCH), and the five markers of Capabilities.
     * Deliberately absent: the remove-only test files of the world sliders and the frame-generation dry-run switches
     * (diag-fg-pair-*). Builds contain those names, and none of them makes a setting available. Unmodifiable.
     */
    static List<String> probeNames() {
        ArrayList<String> names = new ArrayList<String>();
        for (String path : RuntimeMapping.runtimeFiles()) names.add(RuntimeMapping.name(path));
        names.add(RuntimeMapping.name(RuntimeMapping.GSR_SWITCH));
        names.add(Capabilities.GSR1_MARKER);
        names.add(Capabilities.WORLD_LOD_MARKER);
        names.add(Capabilities.FRAME_GENERATION_MARKER);
        names.add(Capabilities.FRAME_GENERATION_AUTO_MARKER);
        names.add(Capabilities.RENDER_STAGE_B_MARKER);
        return Collections.unmodifiableList(names);
    }

    /**
     * The names that occur in the stream directly followed by a zero byte. What stands before a name does not matter
     * (a build may hold a name as the end of a longer path). Reads the stream once, to its end, through a 1 MiB
     * buffer; a name may lie across any two reads.
     */
    static Set<String> scan(InputStream library, Collection<String> names) throws IOException {
        String[] text = names.toArray(new String[0]);
        byte[][] needles = new byte[text.length][];
        boolean[] first = new boolean[256];
        int longest = 0;
        for (int i = 0; i < text.length; i++) {
            needles[i] = text[i].getBytes(StandardCharsets.US_ASCII);
            if (needles[i].length == 0 || needles[i].length >= BUFFER / 2) throw new IllegalArgumentException("bad probe name");
            first[needles[i][0] & 255] = true;
            longest = Math.max(longest, needles[i].length + 1); // the name and its zero byte
        }
        TreeSet<String> found = new TreeSet<String>();
        byte[] buffer = new byte[BUFFER];
        int kept = 0;
        for (;;) {
            int read = library.read(buffer, kept, buffer.length - kept);
            boolean end = read < 0;
            int size = kept + Math.max(read, 0);
            // Start positions checked now: those whose longest possible name and zero byte are already in the buffer.
            // At the end of the stream every remaining position is checked.
            int limit = end ? size : Math.max(size - longest + 1, 0);
            for (int p = 0; p < limit; p++) {
                if (!first[buffer[p] & 255]) continue;
                next:
                for (int n = 0; n < needles.length; n++) {
                    byte[] needle = needles[n];
                    if (p + needle.length >= size) continue; // no room for the zero byte
                    for (int k = 0; k < needle.length; k++) {
                        if (buffer[p + k] != needle[k]) continue next;
                    }
                    if (buffer[p + needle.length] == 0) found.add(text[n]);
                }
            }
            if (end) return found;
            kept = size - limit; // the unchecked tail moves to the front and is completed by the next read
            System.arraycopy(buffer, limit, buffer, 0, kept);
        }
    }

    /**
     * The capabilities of the installed build: from the cache when it still describes the library, otherwise from a
     * scan (about one second for a 340 MB library; call off the UI thread), with the override file applied on top.
     * The only file this may write is settings/capabilities.txt (atomically; the settings folder is created when
     * missing). A failure to write the cache is not an error: the result is still correct.
     * One call at a time (two screens may ask at once; they must not write the cache's temporary file together).
     * IOException: the library exists but cannot be read.
     */
    static synchronized Capabilities load(File filesDir, boolean inputOptionInApp) throws IOException {
        File library = new File(filesDir, LIBRARY);
        if (!library.isFile()) return Capabilities.of(Collections.<String>emptySet(), false, inputOptionInApp);
        long size = library.length(), modified = library.lastModified();
        String header = header(size, modified);
        Set<String> found = readCache(new File(filesDir, RuntimeMapping.CAPABILITIES_CACHE), header);
        if (found == null) {
            InputStream in = new FileInputStream(library);
            try {
                found = scan(in, probeNames());
            } finally {
                in.close();
            }
            // cache only what describes the file as it still is (an installer may have replaced it meanwhile)
            if (library.length() == size && library.lastModified() == modified) writeCache(filesDir, header, found);
        }
        TreeSet<String> names = new TreeSet<String>(found);
        applyOverride(new File(filesDir, OVERRIDE), names);
        return Capabilities.of(names, true, inputOptionInApp);
    }

    /**
     * Line 1 of the cache: the library's size and modification time, and a checksum of the probe names, so that an
     * app update that asks for more names does not trust a cache made with fewer.
     */
    private static String header(long size, long modified) {
        CRC32 crc = new CRC32();
        for (String name : probeNames()) {
            crc.update(name.getBytes(StandardCharsets.US_ASCII));
            crc.update(0);
        }
        return "library " + size + " " + modified + " probes " + Long.toHexString(crc.getValue());
    }

    /** The cached names when the cache is intact and its first line equals header; otherwise null (scan again). */
    private static Set<String> readCache(File cache, String header) {
        byte[] raw = readSmall(cache);
        if (raw == null) return null;
        String[] lines = new String(raw, StandardCharsets.US_ASCII).split("\n", -1);
        if (lines.length < 2 || !lines[0].equals(header) || !lines[lines.length - 1].isEmpty()) return null;
        List<String> known = probeNames();
        TreeSet<String> names = new TreeSet<String>();
        for (int i = 1; i < lines.length - 1; i++) {
            if (!known.contains(lines[i])) return null;
            names.add(lines[i]);
        }
        return names;
    }

    private static void writeCache(File filesDir, String header, Set<String> found) {
        StringBuilder text = new StringBuilder(header).append('\n');
        for (String name : found) text.append(name).append('\n');
        try {
            File directory = new File(filesDir, "settings");
            if (!directory.isDirectory() && !directory.mkdir()) return;
            AtomicFiles.write(SettingsStore.ownedTarget(filesDir, RuntimeMapping.CAPABILITIES_CACHE),
                    text.toString().getBytes(StandardCharsets.US_ASCII));
        } catch (IOException notCached) {
            // the next load scans again
        }
    }

    /** Lines "+name" and "-name"; blank lines, lines starting with '#' and anything else are ignored. */
    private static void applyOverride(File override, Set<String> names) {
        byte[] raw = readSmall(override);
        if (raw == null) return;
        String[] lines = new String(raw, StandardCharsets.US_ASCII).split("\n");
        for (int i = 0; i < lines.length; i++) {
            String line = lines[i].trim();
            if (line.length() < 2 || line.indexOf(' ') >= 0 || line.indexOf('\t') >= 0) continue;
            if (line.charAt(0) == '+') names.add(line.substring(1));
            else if (line.charAt(0) == '-') names.remove(line.substring(1));
        }
    }

    /** A whole file of at most 64 KiB; null when it is missing, larger or unreadable. */
    private static byte[] readSmall(File file) {
        if (!file.isFile() || file.length() > 65536) return null;
        try {
            FileInputStream in = new FileInputStream(file);
            try {
                byte[] buffer = new byte[65536];
                int size = 0;
                for (;;) {
                    int read = in.read(buffer, size, buffer.length - size);
                    if (read <= 0) break;
                    size += read;
                }
                byte[] content = new byte[size];
                System.arraycopy(buffer, 0, content, 0, size);
                return content;
            } finally {
                in.close();
            }
        } catch (IOException unreadable) {
            return null;
        }
    }
}
