package dev.recompbootstrap.smoke;

/**
 * What the About tab's "Captures" section says. Pure Java; AboutScreen counts the pictures and runs the copy
 * (CaptureExport, CaptureMediaStore) off the UI thread and shows what this class returns.
 *
 * A capture is a picture of the game taken in the game window with Select held, then A (CaptureController). The
 * pictures lie in the app's own storage, which no file manager shows; "Copy to Pictures" (A on this section) copies
 * them to the shared folder Pictures/FivePocket Captures, where the Gallery and Files apps show them. Nothing in the
 * app's own storage is changed or removed by the copy.
 */
final class CaptureAbout {
    private CaptureAbout() {}

    static final String SECTION = "Captures";
    static final String FOLDER = "Pictures/FivePocket Captures";
    static final String HOW = "In the game, hold Select and press A to take a picture of the game.";
    /** The store's own limits (CaptureStore), so that nobody learns of them only from a line in the game. */
    static final String KEPT = "The app keeps the newest " + CaptureStore.KEEP_COUNT + " pictures, up to "
            + (CaptureStore.KEEP_BYTES >> 20) + " MB in all. Older ones are removed, also ones that were never copied to"
            + " Pictures: copy the ones you want to keep.";
    static final String COPY = "Copy to Pictures: press A here. The pictures go to " + FOLDER + ", where the Gallery and"
            + " Files apps show them.";
    static final String COUNTING = "Captures: counting…";
    static final String COPYING = "Copying to " + FOLDER + "…";
    static final String NONE = "Captures: none yet.";
    /** After a copy that stopped: the picture could not be written (most likely the shared storage is full). */
    static final String STOPPED_WRITING = " Then the copy stopped: a picture could not be written. Check that this device has"
            + " free storage, then press A again.";
    /** After a copy that stopped: Android did not accept the picture (no shared storage, or no leave to add to it). */
    static final String STOPPED_REFUSED = " Then the copy stopped: this device did not accept a picture in its Pictures"
            + " folder. Press A to try again; if it stops again, restart the device.";

    /** "1 picture", "12 pictures". */
    private static String pictures(long count) {
        return count + (count == 1 ? " picture" : " pictures");
    }

    /**
     * The first line: how many pictures there are, their size, and how many are not yet in the shared folder.
     * summary: CaptureExport.summary's {pictures, bytes, not yet copied}; null while they are counted.
     */
    static String count(long[] summary) {
        if (summary == null) {
            return COUNTING;
        }
        if (summary[0] <= 0) {
            return NONE;
        }
        String size = ShipFiles.megabytes(summary[1]);
        // "have been copied", not "are in": on a device that cannot be asked what its Pictures folder holds now,
        // only the copying is known. Where it can be asked, a copy the player removed counts as not in Pictures.
        String rest = summary[2] <= 0 ? "All of them have been copied to Pictures."
                : summary[2] == summary[0] ? "None of them is in Pictures yet."
                : summary[2] + " of them " + (summary[2] == 1 ? "is" : "are") + " not in Pictures yet.";
        return "Captures: " + pictures(summary[0]) + ", " + size + ". " + rest;
    }

    /** The sentence after a copy. */
    static String result(CaptureExport.Result result) {
        if (result.total == 0) {
            return "There is nothing to copy yet.";
        }
        StringBuilder text = new StringBuilder();
        text.append(result.copied).append(" copied to ").append(FOLDER);
        if (result.already > 0) {
            text.append("; ").append(result.already).append(result.already == 1 ? " was" : " were").append(" already there");
        }
        text.append('.');
        if (result.error != null) {
            // The error names files of the app's own storage: the log has it; the player gets what to do.
            text.append(result.error.contains("Android 10") ? " The copy needs Android 10 or later."
                    : result.refused ? STOPPED_REFUSED : STOPPED_WRITING);
        }
        return text.toString();
    }

    /** True when A on the section has something to do: pictures that are not yet in the shared folder, and no copy running. */
    static boolean canCopy(long[] summary, boolean copying) {
        return !copying && summary != null && summary[2] > 0;
    }

    /** The whole section. last: the result of the copy made on this visit, or null. */
    static String text(long[] summary, boolean copying, CaptureExport.Result last) {
        StringBuilder text = new StringBuilder(count(summary));
        text.append("\n\n").append(HOW).append(' ').append(KEPT);
        if (copying) {
            text.append("\n\n").append(COPYING);
        } else if (last != null) {
            text.append("\n\n").append(result(last));
        } else if (canCopy(summary, false)) {
            text.append("\n\n").append(COPY);
        }
        return text.toString();
    }
}
