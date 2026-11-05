package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;
import static dev.recompbootstrap.smoke.SetupTestKit.expectEquals;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;

/**
 * Run with java -ea. Compares the app's catalog (names, sizes, SHA-256 values) with the repository's local records
 * when they are on this machine. Prints counts only, never a hash.
 */
public final class SetupCatalogTest {
    private static final String RECORDS = System.getProperty("repo.root", ".") + "/local-analysis/";
    private static final String MARKER = "/local-analysis/Disc2-Play/";

    private static boolean rejected(String disc, String path, long bytes, String sha256) {
        try {
            new SetupCatalog.Item(disc, path, bytes, sha256);
            return false;
        } catch (IllegalArgumentException e) {
            return true;
        }
    }

    /** Play-disc path -> "bytes sha256" from one record file (a JSON list of objects with source, bytes, sha256). */
    private static void collect(File record, Map<String, String> into) throws Exception {
        Object parsed = JsonLite.parse(new String(SetupTestKit.read(record), StandardCharsets.UTF_8));
        for (Object o : (List<?>) parsed) {
            Map<?, ?> entry = (Map<?, ?>) o;
            Object source = entry.get("source");
            if (!(source instanceof String) || !((String) source).contains(MARKER)) continue;
            if ("decoded.image.bin".equals(entry.get("role"))) continue;   // made by the desktop builder, not a disc file
            String path = ((String) source).substring(((String) source).indexOf(MARKER) + MARKER.length());
            String value = Long.parseLong(String.valueOf(entry.get("bytes"))) + " " + entry.get("sha256");
            String earlier = into.put(path, value);
            expect(earlier == null || earlier.equals(value), "the two record files agree about " + path);
        }
    }

    public static void main(String[] args) {
        SetupTestKit.requireAssertions();

        check("catalog: every entry is well formed, paths and hashes are unique, the executable comes first", () -> {
            List<SetupCatalog.Item> items = SetupCatalog.items();
            expect(!items.isEmpty(), "not empty");
            expectEquals(SetupCatalog.EXECUTABLE_NAME, items.get(0).path, "first entry");
            expectEquals(SetupCatalog.EXECUTABLE_SHA256, items.get(0).sha256, "the executable's hash is the identification hash");
            Set<String> paths = new HashSet<String>(), hashes = new HashSet<String>();
            long total = 0;
            for (SetupCatalog.Item it : items) {
                expectEquals(SetupCatalog.PLAY, it.disc, "disc of " + it.path);
                expect(paths.add(it.path.toLowerCase(java.util.Locale.ROOT)), "path listed once: " + it.path);
                expect(hashes.add(it.sha256), "hash listed once: " + it.path);
                total += it.bytes;
            }
            expectEquals(total, SetupCatalog.totalBytes(), "totalBytes");
            expect(!SetupCatalog.installDiscAvailable() && !SetupCatalog.spec().coversAllDiscs,
                    "without the install disc an install is never called complete");
            expectEquals(items, SetupCatalog.spec().items, "the spec carries the same catalog");
            boolean fixed = false;
            try {
                items.add(items.get(0));
            } catch (UnsupportedOperationException e) {
                fixed = true;
            }
            expect(fixed, "the list cannot be changed");
        });

        check("catalog: an entry with a bad disc, path, size or hash cannot be made", () -> {
            String sha = SetupCatalog.EXECUTABLE_SHA256;
            expect(!rejected("play", "a.bin", 0, sha), "a good one");
            expect(rejected("other", "a.bin", 1, sha), "disc");
            expect(rejected("play", "", 1, sha), "path");
            expect(rejected("play", "a.bin", -1, sha), "size");
            expect(rejected("play", "a.bin", 1, sha.substring(1)), "short hash");
            expect(rejected("play", "a.bin", 1, sha.toUpperCase(java.util.Locale.ROOT)), "upper-case hash");
            expect(rejected("play", "a.bin", 1, null), "no hash");
        });

        check("host check: the catalog equals the local records of the play disc (names, sizes, hashes)", () -> {
            File one = new File(RECORDS + "app-game-cache-replay-bx9xcvpn/verified-inputs.json");
            File two = new File(RECORDS + "repro-tag-leaf/additional-read-only-files.json");
            if (!one.isFile() || !two.isFile()) {
                SetupTestKit.note("skipped: the local record files are not on this machine");
                return;
            }
            Map<String, String> recorded = new TreeMap<String, String>();
            collect(one, recorded);
            collect(two, recorded);
            Map<String, String> catalog = new TreeMap<String, String>();
            for (SetupCatalog.Item it : SetupCatalog.items()) catalog.put(it.path, it.bytes + " " + it.sha256);
            expectEquals(recorded.keySet(), catalog.keySet(), "the same files");
            for (String path : recorded.keySet()) expect(recorded.get(path).equals(catalog.get(path)), "size and hash of " + path);
            SetupTestKit.note("catalog and records agree on " + catalog.size() + " play-disc files, " + SetupCatalog.totalBytes() + " bytes");
        });

        check("install disc: 8 entries, the four archives and four 14-byte files of the four packages, none on the play list", () -> {
            List<SetupCatalog.Item> items = SetupCatalog.installItems();
            expectEquals(8, items.size(), "eight");
            Set<String> play = new HashSet<String>();
            for (SetupCatalog.Item it : SetupCatalog.items()) play.add(it.sha256);
            long total = 0;
            for (int i = 0; i < items.size(); ++i) {
                SetupCatalog.Item it = items.get(i);
                expectEquals(SetupCatalog.INSTALL, it.disc, "disc");
                expectEquals("545408A70000000" + (i / 2) + "/part" + (i / 2) + (i % 2 == 0 ? ".rpf" : ".timestamp"), it.path, "path " + i);
                if (i % 2 == 1) expectEquals(14L, it.bytes, "a timestamp is 14 bytes");
                expect(!play.contains(it.sha256), "not a play-disc file");
                total += it.bytes;
            }
            expectEquals(8209401912L, total, "8,209,401,912 bytes");
            expectEquals(0x545408A7L, SetupCatalog.game().titleId, "the title of the packages");
            expectEquals(SetupCatalog.EXECUTABLE_SHA256, SetupCatalog.game().executableSha256, "the play disc's program");
            expect(!SetupCatalog.installDiscAvailable() && SetupCatalog.spec().items.size() == 10, "the developer page's catalog is unchanged");
        });

        check("host check: the 18 disc rows of the pinned record are the catalog's two lists (roles title.xex, content-, extra-content-,"
                + " installed-file-)", () -> {
            File record = new File(RECORDS + "app-game-cache-replay-om2astl7/verified-inputs.json");
            if (!record.isFile()) {
                SetupTestKit.note("skipped: the pinned record is not on this machine");
                return;
            }
            Map<String, String> rows = new TreeMap<String, String>(), catalog = new TreeMap<String, String>();
            for (Object o : (List<?>) JsonLite.parse(new String(SetupTestKit.read(record), StandardCharsets.UTF_8))) {
                Map<?, ?> e = (Map<?, ?>) o;
                String role = String.valueOf(e.get("role"));
                if (!(role.equals("title.xex") || role.startsWith("content-") || role.startsWith("extra-content-") || role.startsWith("installed-file-"))) continue;
                rows.put(String.valueOf(e.get("sha256")), (role.startsWith("installed-file-") ? "install " : "play ") + e.get("bytes"));
            }
            for (SetupCatalog.Item it : SetupCatalog.items()) catalog.put(it.sha256, "play " + it.bytes);
            for (SetupCatalog.Item it : SetupCatalog.installItems()) catalog.put(it.sha256, "install " + it.bytes);
            expectEquals(18, rows.size(), "18 rows");
            expectEquals(rows, catalog, "the same entries on the same discs");
            SetupTestKit.note("record and catalog agree on 18 disc entries (10 play, 8 install)");
        });

        SetupTestKit.finish("SetupCatalogTest");
    }
}
