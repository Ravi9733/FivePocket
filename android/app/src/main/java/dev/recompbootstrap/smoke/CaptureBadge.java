package dev.recompbootstrap.smoke;

/**
 * What the game window says in its top right corner about a capture (Select held, then A). Plain Java;
 * CaptureController shows these lines in a small view above the game, which is never part of a picture.
 *
 * Every combo gets an answer on the screen: the picture's number once its file is written, or why there is none.
 * The number is the one in the file's name (cap-NNN.png) and in the log's CAPTURE line.
 */
final class CaptureBadge {
    private CaptureBadge() {}

    /** How long a confirmation stays, and how long a line that says something went wrong. */
    static final long SHORT_MS = 500, LONG_MS = 1500;

    /** The refusals that come before a picture is tried. */
    static final String TOO_SOON = "No capture: wait one second between captures.";
    static final String BUSY = "No capture: the last one is not finished yet.";
    static final String NO_PICTURE_YET = "No capture: the game's picture is not on the screen yet.";
    static final String NO_MEMORY = "No capture: this device is short of memory right now.";
    static final String NO_STORAGE = "Capture not saved: the app's storage could not be written. Check free storage.";
    static final String FAILED = "No capture: the picture could not be taken. Try again.";

    /** The picture's file is written. */
    static String saved(int number) {
        return "Captured " + number;
    }

    /** The picture was taken from the screen but its file could not be written. */
    static String notSaved(int number) {
        return "Capture " + number + " not saved. Check free storage.";
    }

    /** Android gave no picture of the game for this capture. */
    static String noPicture(int number) {
        return "Capture " + number + ": no picture. Try again.";
    }

    /** Older pictures made room (the newest 200 within 800 MB are kept). */
    static String removedOldest(int count) {
        return "Captures full: removed the oldest " + count;
    }

    /** The line for a combo that was refused before a picture was tried: `busy` wins over the one-per-second rule. */
    static String refused(boolean busy) {
        return busy ? BUSY : TOO_SOON;
    }
}
