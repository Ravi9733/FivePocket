package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ALL_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.OLD_TIME;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.V458_NAMES;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.ascii;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.bytes;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.differences;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.library;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.names;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.phoneFiles;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.plus;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.put;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.skip;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.snapshot;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.tempDir;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.Random;
import java.util.Set;
import java.util.TreeSet;

/** Host test of CapabilityScanner: finding the game's input-file names in its library, the cache and the override file. Run with java -ea. */
public final class CapabilityScannerTest {
    private static final String LIBRARY = "private-trace/private-trace-live.so";
    private static final String CACHE = "settings/capabilities.txt";
    private static final String OVERRIDE = "settings/capabilities-override.txt";

    private static byte[] filler(int size) {
        byte[] data = new byte[size];
        Arrays.fill(data, (byte) 'x');
        return data;
    }

    private static void place(byte[] data, int at, String text) {
        byte[] bytes = text.getBytes(StandardCharsets.ISO_8859_1);
        System.arraycopy(bytes, 0, data, at, bytes.length);
    }

    /** A stream that hands out its bytes in small pieces of changing size, as a slow device might. */
    private static InputStream inPieces(final byte[] data, final long seed) {
        return new InputStream() {
            private final Random random = new Random(seed);
            private int at;

            @Override public int read() {
                return at < data.length ? data[at++] & 255 : -1;
            }

            @Override public int read(byte[] buffer, int offset, int length) {
                if (at >= data.length) return -1;
                int count = Math.min(Math.min(length, 1 + random.nextInt(37)), data.length - at);
                System.arraycopy(data, at, buffer, offset, count);
                at += count;
                return count;
            }
        };
    }

    private static Set<String> scan(byte[] data, String... names) throws IOException {
        return CapabilityScanner.scan(new ByteArrayInputStream(data), Arrays.asList(names));
    }

    private static List<String> cacheLines(File files) throws IOException {
        return Arrays.asList(new String(bytes(files, CACHE), StandardCharsets.US_ASCII).split("\n"));
    }

    /** The library file of a phone-like directory, replaced by new content with a chosen modification time. */
    private static void replaceLibrary(File files, byte[] content, long time) throws IOException {
        File library = new File(files, LIBRARY);
        Files.write(library.toPath(), content);
        check(library.setLastModified(time), "set the library's time");
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("CapabilityScannerTest");

        test("probe names: every name the support table asks for is looked for, each once", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                List<String> probes = CapabilityScanner.probeNames();
                check(probes.containsAll(Arrays.asList(ALL_NAMES)), "missing from the probe names: " + names(SettingsCoreTestKit.minus(ALL_NAMES, probes.toArray(new String[0]))));
                same(probes.size(), new TreeSet<String>(probes).size(), "no name twice");
            }
        });

        test("scan: a name counts only when a zero byte follows it", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                byte[] data = filler(5000);
                place(data, 100, "/data/user/0/app/files/private-trace/diag-pace-40\0");  // found; also holds "diag-pace-4" as a prefix only
                place(data, 300, "diag-fps-overlayX");                                     // no zero byte behind it
                place(data, 400, "diag-fg-betwee\0");                                      // cut short
                place(data, 500, "scaler.txt\0");
                place(data, 600, "scaler.txt\0");                                          // twice is still once
                same(names("diag-pace-40", "scaler.txt"),
                        scan(data, "diag-pace-40", "diag-pace-4", "diag-fps-overlay", "diag-fg-between", "scaler.txt", "world-load.txt"), "found");
                same(names(), scan(filler(100), "diag-pace-40"), "nothing in filler");
                same(names(), scan(new byte[0], "diag-pace-40"), "empty stream");
            }
        });

        test("scan: the end of the stream is not a zero byte", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                same(names("diag-fg-ahead"), scan(ascii("xxdiag-fg-ahead\0"), "diag-fg-ahead"), "name and zero byte as the last bytes");
                same(names(), scan(ascii("xxdiag-fg-ahead"), "diag-fg-ahead"), "name as the last bytes, nothing behind it");
                same(names("diag-fg-ahead"), scan(ascii("diag-fg-ahead\0"), "diag-fg-ahead"), "the whole stream is the name");
            }
        });

        test("scan: a name lying across the 1 MiB buffer boundary is found, at every split", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                String name = "diag-fps-overlay";
                int boundary = 1 << 20;
                // the name starts 'before' bytes in front of the boundary: from wholly behind it (0) to wholly in front (length + 1)
                for (int before = 0; before <= name.length() + 1; before++) {
                    byte[] data = filler(boundary + 4096);
                    place(data, boundary - before, name + "\0");
                    same(names(name), scan(data, name, "diag-pace-40"), before + " bytes of the name in front of the boundary");
                }
                // far behind the first boundary (after the buffer was refilled several times)
                byte[] large = filler(3 * boundary + 5000);
                place(large, 2 * boundary - 9, name + "\0");
                place(large, 3 * boundary + 4000, "diag-pace-40\0");
                same(names(name, "diag-pace-40"), scan(large, name, "diag-pace-40", "diag-pace-24"), "later boundaries");
            }
        });

        test("scan: the result does not depend on how the stream hands out its bytes", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                byte[] data = filler(20000);
                Random random = new Random(7);
                String[] present = {"diag-pace-40", "diag-v280-limit30", "world-load.txt", "recomp-cap:gsr1", "diag-fg-ahead"};
                for (int i = 0; i < 40; i++) place(data, 200 + i * 450 + random.nextInt(100), present[i % present.length] + "\0");
                place(data, 19000, "diag-pace-24x");
                place(data, 19500, "diag-fg-between");
                List<String> asked = Arrays.asList(plus(present, "diag-pace-24", "diag-fg-between", "diag-pace-4", "scaler.txt"));
                for (long seed = 1; seed <= 25; seed++) {
                    same(names(present), CapabilityScanner.scan(inPieces(data, seed), asked), "pieces with seed " + seed);
                }
            }
        });

        test("load: scans the library and writes nothing but settings/capabilities.txt", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phoneFiles(tempDir(), V458_NAMES);
                Map<String, String> before = snapshot(files);
                Capabilities capabilities = CapabilityScanner.load(files, true);
                same(new ArrayList<String>(names(V458_NAMES)), capabilities.names(), "names found");
                same(Arrays.asList("added settings", "added settings/capabilities.txt"), differences(before, snapshot(files)), "what load changed");
                List<String> cache = cacheLines(files);
                File library = new File(files, LIBRARY);
                check(cache.get(0).startsWith("library " + library.length() + " " + library.lastModified() + " "), "cache line 1: " + cache.get(0));
                same(new ArrayList<String>(names(V458_NAMES)), cache.subList(1, cache.size()), "cache: one found name per line");
                check(!new File(files, CACHE + ".tmp").exists(), "no temporary file is left");
            }
        });

        test("load: the cache is used while the library's size and time are unchanged", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phoneFiles(tempDir(), V458_NAMES);
                CapabilityScanner.load(files, true);
                // same length, same time, but one name is gone: only a new scan could notice
                byte[] changed = new String(library(V458_NAMES), StandardCharsets.ISO_8859_1).replace("diag-fps-overlay", "diag-xxx-overlay").getBytes(StandardCharsets.ISO_8859_1);
                same(library(V458_NAMES).length, changed.length, "test setup: same length");
                replaceLibrary(files, changed, OLD_TIME);
                Map<String, String> before = snapshot(files);
                check(CapabilityScanner.load(files, true).has("diag-fps-overlay"), "the cached answer is given");
                same(new ArrayList<String>(), differences(before, snapshot(files)), "a load from the cache writes nothing");
            }
        });

        test("load: a different modification time or size makes it scan again", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phoneFiles(tempDir(), V458_NAMES);
                CapabilityScanner.load(files, true);
                byte[] changed = new String(library(V458_NAMES), StandardCharsets.ISO_8859_1).replace("diag-fps-overlay", "diag-xxx-overlay").getBytes(StandardCharsets.ISO_8859_1);
                replaceLibrary(files, changed, OLD_TIME + 60000);
                check(!CapabilityScanner.load(files, true).has("diag-fps-overlay"), "new time: the new content is seen");
                check(cacheLines(files).get(0).contains(" " + (OLD_TIME + 60000) + " "), "the cache now describes the new file");
                check(!cacheLines(files).contains("diag-fps-overlay"), "the cache holds the new result");

                replaceLibrary(files, library(plus(V458_NAMES, "diag-pace-40")), OLD_TIME + 60000);
                Capabilities larger = CapabilityScanner.load(files, true);
                check(larger.has("diag-pace-40") && larger.has("diag-fps-overlay"), "new size, same time: the new content is seen");
            }
        });

        test("load: a cache that is damaged, cut short or made for other probe names is not trusted", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phoneFiles(tempDir(), V458_NAMES);
                CapabilityScanner.load(files, true);
                String good = new String(bytes(files, CACHE), StandardCharsets.US_ASCII);
                String header = good.substring(0, good.indexOf('\n'));
                String[] bad = {
                        header + "\ndiag-fps-overlay\nnot-a-known-name\n",          // a line that is no probe name
                        header + "\ndiag-fps-overlay",                              // last line not finished
                        header.substring(0, header.lastIndexOf(' ')) + " 0\ndiag-pace-40\n", // made with another list of probe names
                        "library 1 2\ndiag-pace-40\n",                              // another file
                        "", "diag-pace-40\n"};
                for (String cache : bad) {
                    put(files, CACHE, cache);
                    Capabilities capabilities = CapabilityScanner.load(files, true);
                    same(new ArrayList<String>(names(V458_NAMES)), capabilities.names(), "after the bad cache <" + cache.replace("\n", "\\n") + ">");
                    same(good, new String(bytes(files, CACHE), StandardCharsets.US_ASCII), "the cache was rewritten");
                }
            }
        });

        test("frame generation through the scan: live only for a library that ends a string with the marker and holds the switch names", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // What the phone's v464 holds (the timing dry run) plus texts that only look like the real thing: the two
                // switch names inside longer names, and the marker in the middle of a log format instead of at its end.
                String[] lookAlikes = SettingsCoreTestKit.plus(SettingsCoreTestKit.plus(SettingsCoreTestKit.V464_NAMES, SetTestKit.DRY_RUN),
                        "diag-fg-between-old", "diag-fg-aheadx", "RENDER_DIRECT_FG cap=recomp-cap:framegen1 pairs=%u");
                File files = phoneFiles(tempDir(), lookAlikes);
                Capabilities none = CapabilityScanner.load(files, true);
                same(new ArrayList<String>(names(SettingsCoreTestKit.V464_NAMES)), none.names(), "names found: v464's, nothing of frame generation");
                check(!none.anyChoice("frameGeneration"), "the row is not live");
                same("Not in this version", none.reason("frameGeneration", 1), "its reason");

                // The library of the design: the marker ends a string (as the two existing markers do) and both switch names are there.
                File announced = phoneFiles(tempDir(), SettingsCoreTestKit.plus(SettingsCoreTestKit.minus(SetTestKit.FRAMEGEN_NAMES, SetTestKit.FRAMEGEN_MARKER),
                        "RENDER_DIRECT_FG generator=1 cap=recomp-cap:framegen1"));
                Capabilities live = CapabilityScanner.load(announced, true);
                check(live.valueSupported("frameGeneration", 1) && live.valueSupported("frameGeneration", 2), "both orders are offered: " + live.names());
                check(CapabilityScanner.load(announced, true).valueSupported("frameGeneration", 2), "and from the cache as well");

                // The lead's override file switches the row on for a trial on a library that is not announced yet, and off again.
                put(files, OVERRIDE, "+recomp-cap:framegen1\n+diag-fg-between\n+diag-fg-ahead\n");
                check(CapabilityScanner.load(files, true).valueSupported("frameGeneration", 1), "override: on");
                put(announced, OVERRIDE, "-recomp-cap:framegen1\n");
                same("Not in this version", CapabilityScanner.load(announced, true).reason("frameGeneration", 1), "override: off again");
            }
        });

        test("app update: the cache the previous app version wrote is not trusted, because this version looks for more names", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                // The phone's capabilities.txt was written by app-settings-7, whose probe list had no frame-generation
                // switch names (its header ends "probes bb4a2ac3": app-ui-5/apk/DEVICE-STEPS.md step 3). The library
                // here holds the generator; its size and time are the ones that cache names.
                File files = phoneFiles(tempDir(), SetTestKit.FRAMEGEN_NAMES);
                File library = new File(files, LIBRARY);
                StringBuilder old = new StringBuilder("library " + library.length() + " " + library.lastModified() + " probes bb4a2ac3\n");
                for (String name : names(SettingsCoreTestKit.V464_NAMES)) old.append(name).append('\n');
                put(files, CACHE, old.toString());
                Capabilities capabilities = CapabilityScanner.load(files, true);
                check(capabilities.valueSupported("frameGeneration", 1), "the library was scanned again: the generator is seen");
                String header = cacheLines(files).get(0);
                check(header.startsWith("library " + library.length() + " ") && !header.endsWith(" bb4a2ac3"), "the cache was rewritten under this version's probe list: " + header);
                check(cacheLines(files).contains("diag-fg-between") && cacheLines(files).contains("recomp-cap:framegen1"), "and holds what the scan found");
            }
        });

        test("load: the override file adds and removes names, is never written, and never reaches the cache", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phoneFiles(tempDir(), V458_NAMES);
                put(files, OVERRIDE, "# set by the lead\n+diag-pace-40\n-diag-fps-overlay\n\n+recomp-cap:gsr1 \r\nplain words\n+ two words\n+\n-\n-not-there\n");
                String overrideBefore = snapshot(files).get(OVERRIDE);
                for (int round = 0; round < 2; round++) { // the second round answers from the cache
                    Capabilities capabilities = CapabilityScanner.load(files, true);
                    TreeSet<String> expected = new TreeSet<String>(names(V458_NAMES));
                    expected.remove("diag-fps-overlay");
                    expected.add("diag-pace-40");
                    expected.add("recomp-cap:gsr1");
                    same(new ArrayList<String>(expected), capabilities.names(), "names with the override, round " + round);
                    check(capabilities.valueSupported("frameCap", 2) && !capabilities.valueSupported("fpsOverlay", 1), "the override decides what is offered");
                }
                same(overrideBefore, snapshot(files).get(OVERRIDE), "the override file is untouched");
                List<String> cache = cacheLines(files);
                same(new ArrayList<String>(names(V458_NAMES)), cache.subList(1, cache.size()), "the cache holds the scan, not the override");
            }
        });

        test("load: without a game library nothing is supported and nothing is written", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phoneFiles(tempDir(), V458_NAMES);
                check(new File(files, LIBRARY).delete(), "test setup: remove the stand-in library");
                put(files, OVERRIDE, "+diag-fps-overlay\n");
                Map<String, String> before = snapshot(files);
                Capabilities capabilities = CapabilityScanner.load(files, true);
                check(!capabilities.valueSupported("fpsOverlay", 1), "nothing but the defaults, even with an override");
                check(!capabilities.valueSupported("scaler", 3), "not even a value that needs no file name");
                same("Game code is not installed", capabilities.reason("fpsOverlay", 1), "reason");
                same(new ArrayList<String>(), differences(before, snapshot(files)), "nothing written");
            }
        });

        test("load: a settings folder that is a symbolic link is not written through", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File files = phoneFiles(tempDir(), V458_NAMES);
                Files.createSymbolicLink(new File(files, "settings").toPath(), new File(files, "saves").toPath());
                Map<String, String> before = snapshot(files);
                Capabilities capabilities = CapabilityScanner.load(files, true);
                check(capabilities.has("diag-fps-overlay"), "the scan result is still returned");
                same(new ArrayList<String>(), differences(before, snapshot(files)), "nothing written (above all not into saves)");
            }
        });

        final File real = realLibrary();
        if (real == null) {
            skip("the real v458 game library holds exactly the seven names of design 3.3", "library file not found; pass -Dv458.library=<path>");
        } else {
            test("the real v458 game library holds exactly the seven names of design 3.3", new SettingsCoreTestKit.Case() {
                @Override public void run() throws IOException {
                    long start = System.nanoTime();
                    Set<String> found;
                    InputStream in = new FileInputStream(real);
                    try {
                        found = CapabilityScanner.scan(in, CapabilityScanner.probeNames());
                    } finally {
                        in.close();
                    }
                    System.out.println("      scanned " + real.length() + " bytes in " + (System.nanoTime() - start) / 1000000 + " ms: " + found);
                    same(names(V458_NAMES), found, "names in the v458 library");
                }
            });
        }

        SettingsCoreTestKit.end();
    }

    /** The v458 host build of the game library: -Dv458.library=..., or the repository path when the test runs inside the repository. */
    private static File realLibrary() {
        String given = System.getProperty("v458.library");
        if (given != null) return new File(given).isFile() ? new File(given) : null;
        File directory = new File("").getAbsoluteFile();
        for (int up = 0; up < 6 && directory != null; up++, directory = directory.getParentFile()) {
            File candidate = new File(directory, "local-analysis/attempts/v458/build/libhost_guest_trace-attempt-v458.so");
            if (candidate.isFile()) return candidate;
        }
        return null;
    }
}
