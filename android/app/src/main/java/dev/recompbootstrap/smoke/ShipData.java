package dev.recompbootstrap.smoke;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Which of the game data files this app version pins are in place. Pure Java, read-only.
 *
 * The rule, asked in one place so that the first screen, Play and the launcher's state line cannot disagree:
 *   - every pinned file (ShipConfig.pinnedData) is a regular file of exactly the pinned size at its place under
 *     files/private-trace (the image beside the bundled start request). A symbolic link is not the file, and a file
 *     that is only reached through a link under a folder's name is not in place either (ShipFiles.regularFileIn):
 *     the two-disc copy and the first start use the same rule, so that none of them counts what another does not;
 *   - every pinned file that travels inside the APK (ShipConfig.bundledData) and every pinned file of at most
 *     64 MiB also has the pinned SHA-256. The larger files (up to 2.8 GB each, 16.3 GB in all) were hashed when they
 *     were copied; afterwards they are compared by size and are not read.
 * A file that is absent is told apart from one that is there but is not the pinned file.
 */
final class ShipData {
    /** Pinned files up to this size are compared by SHA-256 every time the rule is asked. */
    static final long HASH_LIMIT = 64L << 20;

    /** How many files this app version pins. */
    final int pinned;
    /** Pinned files that are not there as a regular file, in the record's order. */
    final List<ShipConfig.DataFile> absent;
    /** Pinned files that are there with another size or, where hashed, with other content. */
    final List<ShipConfig.DataFile> different;
    /** Of absent and different together: how many travel inside the APK (the first start puts those back). */
    final int bundledAmongThem;
    private final int notBundledAbsent, notBundledTotal;
    /**
     * The files that come from the player's own discs (the pinned files the APK does not carry): how many there are,
     * how many of them are absent or are not the pinned file, and the bytes of those. What the menu counts in.
     */
    final int discFiles, discFilesMissing;
    final long discBytesMissing;

    private ShipData(int pinned, List<ShipConfig.DataFile> absent, List<ShipConfig.DataFile> different, int bundledAmongThem,
            int notBundledAbsent, int notBundledTotal, int discFilesMissing, long discBytesMissing) {
        this.pinned = pinned;
        this.absent = Collections.unmodifiableList(absent);
        this.different = Collections.unmodifiableList(different);
        this.bundledAmongThem = bundledAmongThem;
        this.notBundledAbsent = notBundledAbsent;
        this.notBundledTotal = notBundledTotal;
        this.discFiles = notBundledTotal;
        this.discFilesMissing = discFilesMissing;
        this.discBytesMissing = discBytesMissing;
    }

    /** True when every pinned file is in place by the rule of this class. */
    boolean complete() {
        return absent.isEmpty() && different.isEmpty();
    }

    /** How many pinned files are absent or are not the pinned file. */
    int missing() {
        return absent.size() + different.size();
    }

    /** The bytes of the pinned files that are absent or are not the pinned file. */
    long missingBytes() {
        long total = 0;
        for (ShipConfig.DataFile file : absent) total += file.bytes;
        for (ShipConfig.DataFile file : different) total += file.bytes;
        return total;
    }

    /** null when nothing is missing; otherwise one sentence. */
    String sentence() {
        if (complete()) return null;
        // Nothing from the player's discs is here yet and nothing else is wrong: the state after a first start.
        if (different.isEmpty() && bundledAmongThem == 0 && notBundledTotal > 0 && notBundledAbsent == notBundledTotal) {
            return "Your game data is not on this device yet.";
        }
        return counted();
    }

    /** The same in numbers, whatever is missing: "18 of 39 game data files are missing." Only when not complete. */
    String counted() {
        return missing() + " of " + pinned + " game data files are missing"
                + (different.isEmpty() ? "." : " or are not the files this app version was made for.");
    }

    /** Where a pinned file belongs under the app's files directory. */
    static File place(ShipConfig config, File filesDir, String path) {
        return new File(new File(filesDir, InstallState.PRIVATE_TRACE), ShipDataManifest.target(path, config.runId));
    }

    /**
     * Looks at every pinned file: its name and size, and for the bundled pieces and the files up to 64 MiB its
     * SHA-256 (the real build: about 74 MB are read; call it off the UI thread). Never writes, never throws: a
     * file that has to be read and cannot be counts as not the pinned file.
     */
    static ShipData look(ShipConfig config, File filesDir) {
        List<ShipConfig.DataFile> absent = new ArrayList<ShipConfig.DataFile>();
        List<ShipConfig.DataFile> different = new ArrayList<ShipConfig.DataFile>();
        int bundledAmongThem = 0, notBundledAbsent = 0, discFilesMissing = 0;
        long discBytesMissing = 0;
        for (ShipConfig.DataFile pinned : config.pinnedData) {
            File file = place(config, filesDir, pinned.path);
            boolean bundled = config.bundled(pinned.path) != null;
            boolean isAbsent = !ShipFiles.regularFileIn(filesDir, file);
            boolean isDifferent = false;
            if (!isAbsent) {
                isDifferent = file.length() != pinned.bytes;
                if (!isDifferent && (bundled || pinned.bytes <= HASH_LIMIT)) {
                    try {
                        isDifferent = !pinned.sha256.equals(ShipFiles.sha256Of(file));
                    } catch (IOException | RuntimeException unreadable) {
                        isDifferent = true;
                    }
                }
            }
            if (isAbsent) absent.add(pinned);
            if (isDifferent) different.add(pinned);
            if ((isAbsent || isDifferent) && bundled) ++bundledAmongThem;
            if (isAbsent && !bundled) ++notBundledAbsent;
            if ((isAbsent || isDifferent) && !bundled) {
                ++discFilesMissing;
                discBytesMissing += pinned.bytes;
            }
        }
        return new ShipData(config.pinnedData.size(), absent, different, bundledAmongThem, notBundledAbsent,
                config.notBundled.size(), discFilesMissing, discBytesMissing);
    }
}
