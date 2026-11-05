package dev.recompbootstrap.smoke;

import java.io.File;
import java.util.Locale;

/**
 * Everything the first-run setup screen decides, without any Android class: the five steps and their states, the
 * focus, what CONFIRM / BACK / LEFT / RIGHT do, the two questions ("Where is your copy of the play disc?",
 * "Stop copying?") and every text. Pure Java.
 * SetupScreen draws from the getters, forwards commands to nav(), and carries out takeAction().
 *
 * The rule for every text of a step (detail): it says what is true now and what the player can do next, in words for
 * a person who has never seen this project. What this app version cannot do (read the install disc, start the game
 * from what this page installs) is said before the player spends time on it, with the way that works instead: the
 * install kit.
 */
final class SetupModel {
    static final int STEP_PLAY = 0, STEP_INSTALL = 1, STEP_COPY = 2, STEP_IMPORT = 3, STEP_DONE = 4, STEPS = 5;
    static final int TODO = 0, WORKING = 1, DONE = 2, PROBLEM = 3, UNAVAILABLE = 4;
    static final int ACT_NONE = 0, ACT_PICK_PLAY_IMAGE = 1, ACT_PICK_PLAY_FOLDER = 2, ACT_START_COPY = 3,
            ACT_PICK_BUNDLE = 4, ACT_STOP_WORK = 5, ACT_LEAVE = 6;
    static final int OVERLAY_NONE = 0, OVERLAY_STOP = 1, OVERLAY_SOURCE = 2;
    static final int BUTTON_STOP = 0, BUTTON_CONTINUE = 1;
    static final int BUTTON_IMAGE = 0, BUTTON_FOLDER = 1, BUTTON_CANCEL = 2;

    private static final String[] LABELS = {
            "1  Choose your play disc", "2  Choose your install disc", "3  Copy game files", "4  Import game code", "5  Finish"};
    private static final String[] WORDS = {"To do", "Working", "Done", "Problem", "Not available"};
    private static final String[] STOP_BUTTONS = {"Stop", "Continue"};
    private static final String[] SOURCE_BUTTONS = {"Image file", "Folder", "Cancel"};

    // ---- the texts that more than one place uses (and that the tests name) ----------------------------------------

    /** The one way to a game that starts, in this version. The lead's kit lane owns what it is called. */
    static final String KIT = "the install kit";
    static final String KIT_DOES = "The install kit installs the whole game, both discs, from a computer with this device connected by USB.";
    static final String TEXT_CHECKING = "Looking at what is already on this device.";
    static final String TEXT_INTRO = "GTA V for Xbox 360 comes on two discs. This page copies the play disc (Disc 2) from an image"
            + " file or a folder on this device. It cannot read the install disc (Disc 1) yet, so it cannot finish an install"
            + " by itself: to play now, use " + KIT + ". A: choose your play disc.";
    static final String TEXT_INSTALL_DISC = "Not available in this version: this app cannot read the install disc (Disc 1) yet."
            + " The game needs that disc's files as well as the play disc's. " + KIT_DOES;
    static final String TEXT_READING = "Reading your disc copy and checking that it is the supported game. This takes a moment.";
    static final String TEXT_NOTHING_CHOSEN = "Nothing was chosen. A: open the file chooser again. If no file chooser opened, this"
            + " device has none; then nothing can be chosen on this page, and " + KIT + " is the way to install.";
    static final String TEXT_IMPORT = "The game's program has to be rebuilt for this device. That is done on a computer, from your own"
            + " disc; the build tool there writes one file, the code bundle (its name normally ends in .bundle). Copy that file to this"
            + " device, then press A and choose it.";
    static final String TEXT_NOT_FINISHED = "The play disc's files and the game code are on this device. That is all this page can"
            + " install in this version: without the install disc's files the game does not start from this install. To play now,"
            + " use " + KIT + ". A or B: back to the main menu.";
    static final String TEXT_FINISHED = "Setup is complete. A or B: back to the main menu, then choose Play.";
    static final String TEXT_SOURCE_QUESTION = "Where is your copy of the play disc (Disc 2)? Image file: one .iso file of the whole disc."
            + " Folder: a folder that holds the disc's files (default.xex and the .rpf files). The system's file chooser opens"
            + " next; if the controller does not move in it, use the touch screen.";
    static final String TEXT_STOP_COPY = "Stop copying? The files that are finished are kept. The file that is being copied now starts"
            + " again next time.";
    static final String TEXT_STOP_IMPORT = "Stop the import? Nothing of it is kept: it starts again from the beginning next time.";
    private static final String NEVER_CHANGED = " This page never changes such an install, so nothing can be chosen here.";

    /** What is in place on the device. Gathered by inspect(), which only reads. */
    static final class Disk {
        /** What closes the setup (InstallGuard.existing); never null. */
        final InstallGuard.Existing existing;
        final int filesPresent, filesTotal;
        /** bytesMissing: of the catalog's files. freeBytes: what a copy can count on (leftovers of an interrupted run included). */
        final long bytesMissing, freeBytes;
        final boolean bundlePresent;
        final boolean coversAllDiscs;

        Disk(InstallGuard.Existing existing, int filesPresent, int filesTotal, long bytesMissing, long freeBytes, boolean bundlePresent,
                boolean coversAllDiscs) {
            this.existing = existing == null ? new InstallGuard.Existing(false, false, false, false) : existing;
            this.filesPresent = filesPresent;
            this.filesTotal = filesTotal;
            this.bytesMissing = bytesMissing;
            this.freeBytes = freeBytes;
            this.bundlePresent = bundlePresent;
            this.coversAllDiscs = coversAllDiscs;
        }
    }

    static Disk inspect(File filesDir) {
        return inspect(filesDir, SetupCatalog.spec(), VerifiedExtractor.DISK);
    }

    /** Read-only: creates and changes nothing. */
    static Disk inspect(File filesDir, SetupCatalog.Spec spec, VerifiedExtractor.Space space) {
        File root = InstallGuard.root(filesDir);
        int present = 0;
        long missing = 0;
        for (SetupCatalog.Item item : spec.items) {
            if (VerifiedExtractor.present(root, item)) ++present; else missing += item.bytes;
        }
        boolean bundle = new File(new File(root, CodeBundleImporter.BUNDLE), CodeBundle.MANIFEST_NAME).isFile();
        return new Disk(InstallGuard.existing(filesDir), present, spec.items.size(), missing,
                VerifiedExtractor.freeWithLeftovers(root, space), bundle, spec.coversAllDiscs);
    }

    private Disk disk = new Disk(null, 0, 0, 0, 0, false, false);
    private boolean inspected;
    private int focus;
    private int overlay = OVERLAY_NONE, overlayFocus = 0;
    private int pending = ACT_NONE;
    private int working = -1;              // the step that is working, or -1
    private boolean stopping;
    private int playState = TODO, copyState = TODO, importState = TODO;
    private String playMessage, playNext;
    /** Why the last copy / import did not finish (an outcome with an error), or null. */
    private VerifiedExtractor.Outcome copyFailure, importFailure;
    /** The last copy / import was stopped by the player and nothing was tried since. */
    private boolean copyStopped, importStopped;
    /** The step whose file chooser was closed without a choice, or -1. */
    private int nothingChosen = -1;
    private long bytesDone, bytesTotal, startBytes = -1, startMs;
    private int filesDone, filesTotal;
    private String currentPath = "";
    private long nowMs;

    /** Takes what inspect() found. The first time, the focus goes to the first step that still needs the user. */
    void setDisk(Disk found) {
        disk = found;
        if (working < 0) {
            if (copyComplete()) copyState = DONE;
            else if (copyState == DONE) copyState = TODO;
            if (disk.bundlePresent) importState = DONE;
            else if (importState == DONE) importState = TODO;
        }
        if (!inspected) {
            inspected = true;
            focus = STEP_PLAY;
            if (!readOnly()) {
                if (copyComplete()) focus = disk.bundlePresent ? STEP_DONE : STEP_IMPORT;
            }
        }
    }

    boolean readOnly() {
        return disk.existing.any();
    }

    private boolean copyComplete() {
        return disk.filesTotal > 0 && disk.filesPresent == disk.filesTotal;
    }

    /** Everything this page can install is in place. */
    private boolean partsInPlace() {
        return copyState == DONE && importState == DONE;
    }

    boolean busy() {
        return working >= 0;
    }

    int focused() {
        return focus;
    }

    int overlay() {
        return overlay;
    }

    int overlayFocus() {
        return overlayFocus;
    }

    /** The question of the open overlay. */
    String overlayText() {
        if (overlay == OVERLAY_SOURCE) return TEXT_SOURCE_QUESTION;
        return working == STEP_IMPORT ? TEXT_STOP_IMPORT : TEXT_STOP_COPY;
    }

    /** The buttons of the open overlay, left to right. Empty when none is open. */
    String[] overlayButtons() {
        if (overlay == OVERLAY_STOP) return STOP_BUTTONS.clone();
        if (overlay == OVERLAY_SOURCE) return SOURCE_BUTTONS.clone();
        return new String[0];
    }

    /** Touch: a button of the open overlay was tapped. */
    void overlayTap(int button) {
        if (overlay == OVERLAY_NONE || button < 0 || button >= overlayButtons().length) return;
        overlayFocus = button;
        activateOverlay();
    }

    private void activateOverlay() {
        int kind = overlay;
        overlay = OVERLAY_NONE;
        if (kind == OVERLAY_STOP) {
            if (overlayFocus == BUTTON_STOP && busy()) {
                stopping = true;
                pending = ACT_STOP_WORK;
            }
        } else if (kind == OVERLAY_SOURCE && playActionable()) {
            if (overlayFocus == BUTTON_IMAGE) pending = ACT_PICK_PLAY_IMAGE;
            else if (overlayFocus == BUTTON_FOLDER) pending = ACT_PICK_PLAY_FOLDER;
        }
    }

    /** Returns the action the screen must carry out now and forgets it. */
    int takeAction() {
        int action = pending;
        pending = ACT_NONE;
        return action;
    }

    /** Touch: put the focus on a step. */
    void focus(int step) {
        if (overlay == OVERLAY_NONE && step >= 0 && step < STEPS) focus = step;
    }

    /** A NavInput command. Returns true if handled; false for BACK means "leave this screen". */
    boolean nav(int command, boolean repeat) {
        if (overlay != OVERLAY_NONE) {
            // an open question takes every command; LEFT / RIGHT stop at the ends
            if (command == NavInput.LEFT) overlayFocus = Math.max(0, overlayFocus - 1);
            else if (command == NavInput.RIGHT) overlayFocus = Math.min(overlayButtons().length - 1, overlayFocus + 1);
            else if (command == NavInput.BACK && !repeat) overlay = OVERLAY_NONE;
            else if (command == NavInput.CONFIRM && !repeat) activateOverlay();
            return true;
        }
        switch (command) {
            case NavInput.UP:
                if (focus > 0) --focus; else if (!repeat) focus = STEPS - 1;
                return true;
            case NavInput.DOWN:
                if (focus < STEPS - 1) ++focus; else if (!repeat) focus = 0;
                return true;
            case NavInput.CONFIRM:
                if (!repeat) confirm();
                return true;
            case NavInput.BACK:
                if (repeat) return true;
                if (!busy()) return false;
                // Reading the disc writes nothing, and work that was told to stop only winds down: the page can be
                // left, so that a source that never answers cannot hold the player here.
                if (working == STEP_PLAY || stopping) return false;
                overlay = OVERLAY_STOP;
                overlayFocus = BUTTON_STOP;   // the player pressed B to leave, so A confirms that
                return true;
            default:
                return false;
        }
    }

    private boolean playActionable() {
        return !readOnly() && !busy() && copyState != DONE;
    }

    private void confirm() {
        if (!inspected || readOnly() || busy()) return;
        switch (focus) {
            case STEP_PLAY:
                if (playActionable()) {
                    overlay = OVERLAY_SOURCE;
                    overlayFocus = BUTTON_IMAGE;
                }
                break;
            case STEP_COPY:
                if (copyState != DONE && playState == DONE) pending = ACT_START_COPY;
                break;
            case STEP_IMPORT:
                if (importState != DONE && copyState == DONE) pending = ACT_PICK_BUNDLE;
                break;
            case STEP_DONE:
                if (partsInPlace()) pending = ACT_LEAVE;
                break;
            default:
                break;
        }
    }

    // ---- what the screen reports back -------------------------------------------------------------------------

    /** The file chooser of a step (STEP_PLAY or STEP_IMPORT) was closed without a choice, or none could be opened. */
    void nothingChosen(int step) {
        if (!busy()) nothingChosen = step;
    }

    /** The disc was chosen and is being read. */
    void identifyStarted() {
        working = STEP_PLAY;
        playState = WORKING;
    }

    void playDiscIdentified(DiscIdentifier.Result result) {
        if (working == STEP_PLAY) working = -1;
        playState = result.kind == DiscIdentifier.Kind.SUPPORTED_PLAY_DISC ? DONE : PROBLEM;
        playMessage = result.message;
        playNext = result.next;
        if (playState == DONE && focus == STEP_PLAY) focus = STEP_COPY;
    }

    void workStarted(int step, long now) {
        working = step;
        stopping = false;
        nothingChosen = -1;
        startMs = now;
        nowMs = now;
        startBytes = -1;
        bytesDone = 0;
        bytesTotal = 0;
        filesDone = 0;
        filesTotal = 0;
        currentPath = "";
        if (step == STEP_COPY) copyState = WORKING; else importState = WORKING;
    }

    void progress(long done, long total, int files, int filesOf, String current, long now) {
        if (!busy()) return;
        if (startBytes < 0 && total > 0) startBytes = done;   // bytes that were already in place when this run began
        bytesDone = done;
        bytesTotal = total;
        filesDone = files;
        filesTotal = filesOf;
        currentPath = current == null ? "" : current;
        nowMs = now;
    }

    /** Call after setDisk() with a fresh inspection, so that Done reflects what is really in place. */
    void workFinished(VerifiedExtractor.Outcome outcome) {
        int step = working;
        working = -1;
        stopping = false;
        overlay = OVERLAY_NONE;
        if (step < 0) return;
        boolean inPlace = step == STEP_COPY ? copyComplete() : disk.bundlePresent;
        int state = inPlace ? DONE : outcome.error != null ? PROBLEM : TODO;
        VerifiedExtractor.Outcome failure = state == PROBLEM ? outcome : null;
        boolean stopped = state == TODO && outcome.cancelled;
        if (step == STEP_COPY) {
            copyState = state;
            copyFailure = failure;
            copyStopped = stopped;
            if (state == DONE && focus == STEP_COPY) focus = STEP_IMPORT;
        } else {
            importState = state;
            importFailure = failure;
            importStopped = stopped;
            if (state == DONE && focus == STEP_IMPORT) focus = STEP_DONE;
        }
    }

    // ---- what the screen draws ---------------------------------------------------------------------------------

    String title() {
        return "Set up game files";
    }

    String label(int step) {
        return LABELS[step];
    }

    int state(int step) {
        if (!inspected) return TODO;
        // An install this page did not make is not described by its steps; a complete one of its own is.
        if (readOnly()) return disk.existing.recordComplete && !computerInstall() ? DONE : UNAVAILABLE;
        switch (step) {
            case STEP_PLAY: return copyState == DONE && playState != WORKING ? DONE : playState;
            case STEP_INSTALL: return UNAVAILABLE;
            case STEP_COPY: return copyState == TODO && playState == DONE && lowSpace() ? PROBLEM : copyState;
            case STEP_IMPORT: return importState;
            default: return !partsInPlace() ? TODO : disk.coversAllDiscs ? DONE : UNAVAILABLE;
        }
    }

    String stateWord(int step) {
        return WORDS[state(step)];
    }

    /** True when CONFIRM on the step would do something now (the row is drawn enabled). */
    boolean actionable(int step) {
        if (!inspected || readOnly() || busy()) return false;
        switch (step) {
            case STEP_PLAY: return playActionable();
            case STEP_COPY: return copyState != DONE && playState == DONE;
            case STEP_IMPORT: return importState != DONE && copyState == DONE;
            case STEP_DONE: return partsInPlace();
            default: return false;
        }
    }

    /** The help text of a step: what is true now and what can be done next. */
    String detail(int step) {
        if (!inspected) return TEXT_CHECKING;
        if (readOnly()) return readOnlyText();
        switch (step) {
            case STEP_PLAY: return playText();
            case STEP_INSTALL: return TEXT_INSTALL_DISC;
            case STEP_COPY: return copyText();
            case STEP_IMPORT: return importText();
            default:
                if (!partsInPlace()) return "Finish the steps above first.";
                return disk.coversAllDiscs ? TEXT_FINISHED : TEXT_NOT_FINISHED;
        }
    }

    private boolean computerInstall() {
        return disk.existing.computerData || disk.existing.computerCode;
    }

    /** Why nothing can be changed here, and what can be done instead. */
    private String readOnlyText() {
        InstallGuard.Existing e = disk.existing;
        if (e.computerData && e.computerCode) {
            return "The game on this device was installed from a computer." + NEVER_CHANGED
                    + " To play, go back (B) and choose Play. If Play is not offered, run " + KIT + " again to complete the install.";
        }
        if (e.computerData) {
            return "Game files were put on this device from a computer, but the game code is not there." + NEVER_CHANGED
                    + " Run " + KIT + " again to complete the install.";
        }
        if (e.computerCode) {
            return "Game code was put on this device from a computer, but the game's data files are not there." + NEVER_CHANGED
                    + " Run " + KIT + " again to complete the install.";
        }
        if (e.recordUnreadable) {
            return "This page's own record of an earlier setup cannot be read: it is damaged or was written by a newer version of"
                    + " this app. To be safe, this page changes nothing. Install the game with " + KIT
                    + ", or install the newer app version again.";
        }
        return "A complete install made by this page is on this device; nothing more can be done here. B: back to the main menu,"
                + " then choose Play.";
    }

    private String playText() {
        if (playState == WORKING) return TEXT_READING;
        if (copyState == DONE) {
            return "All " + disk.filesTotal + " files of the play disc are on this device. The image file or folder you chose is not"
                    + " needed by this page again.";
        }
        // A is named only where it does something: while the copy runs the disc cannot be chosen again.
        if (playState == DONE) {
            return playMessage + (playActionable() ? " Next: step 3 copies its files. A: choose a different disc copy."
                    : " Step 3 is copying its files.");
        }
        if (playState == PROBLEM) return playMessage + (playNext == null ? "" : " " + playNext) + " A: choose again.";
        // Only while nothing was ever chosen: a chooser that gave a result before does exist.
        if (nothingChosen == STEP_PLAY) return TEXT_NOTHING_CHOSEN;
        String space = lowSpace() ? " " + lowSpaceSentence() : "";
        if (disk.filesPresent > 0) {
            return disk.filesPresent + " of " + disk.filesTotal + " files of the play disc " + isAre() + " already on this device and checked."
                    + " To copy the rest, choose the same disc copy again: its image file or its folder. A: choose." + space;
        }
        return TEXT_INTRO + space;
    }

    private String copyText() {
        if (copyState == WORKING) return stopping ? "Stopping\u2026" : progressText("Copying");
        if (copyState == DONE) {
            return "All " + disk.filesTotal + " files of the play disc are on this device; each one matched the supported disc when it was copied.";
        }
        if (copyState == PROBLEM) return failureText(copyFailure, STEP_COPY);
        String have = disk.filesPresent > 0 ? disk.filesPresent + " of " + disk.filesTotal + " files " + isAre() + " already copied and checked. " : "";
        if (playState != DONE) {
            return have + (disk.filesPresent > 0 ? "To copy the rest, choose your play disc again (step 1). " : "Choose your play disc first (step 1). ")
                    + (lowSpace() ? lowSpaceText(spaceNeed(), disk.freeBytes, "before you start the copy") : spaceSentence());
        }
        String stopped = copyStopped ? "Stopped. " : "";
        if (lowSpace()) return stopped + have + lowSpaceText(spaceNeed(), disk.freeBytes, "then press A to try again");
        if (copyStopped || disk.filesPresent > 0) {
            return stopped + have + (copyStopped ? "The file that was being copied starts again. " : "") + spaceSentence() + " A: continue.";
        }
        return "This step copies the play disc's files to this device and checks each one against the supported disc. " + spaceSentence()
                + " B stops it at any time: finished files are kept. A: start.";
    }

    private String importText() {
        if (importState == WORKING) return stopping ? "Stopping\u2026" : progressText("Importing");
        if (importState == DONE) return "The game code is on this device and checked.";
        if (importState == PROBLEM) return failureText(importFailure, STEP_IMPORT);
        if (copyState != DONE) return "Copy the game files first (step 3).";
        if (nothingChosen == STEP_IMPORT) return TEXT_NOTHING_CHOSEN;
        if (importStopped) return "Stopped: nothing was imported. A: choose the bundle file again.";
        return TEXT_IMPORT;
    }

    /** "is" for one file in place, else "are". */
    private String isAre() {
        return disk.filesPresent == 1 ? "is" : "are";
    }

    /** What happened (the work's own sentence, which names the file) and what to do about it. */
    private String failureText(VerifiedExtractor.Outcome failure, int step) {
        boolean copy = step == STEP_COPY;
        String kept = copy && disk.filesPresent > 0 ? (disk.filesPresent == 1 ? " The 1 file already copied is kept."
                : " The " + disk.filesPresent + " files already copied are kept.") : "";
        switch (failure.problem) {
            case NO_SPACE:
                if (!copy) return lowSpaceText(failure.needBytes, failure.freeBytes, "then press A and choose the bundle file again");
                // The amounts are this moment's, not those of the failure: the player may have made room since.
                if (!lowSpace()) return "The copy stopped for lack of free space. There is enough now. " + spaceSentence() + " A: continue." + kept;
                return "The copy stopped. " + lowSpaceText(spaceNeed(), disk.freeBytes, "then press A to continue") + kept;
            case DISC_COPY_BAD:
                return failure.error + " Your disc copy is damaged or is not the supported disc. Make a new copy of the play disc and"
                        + " choose it in step 1." + kept;
            case SOURCE_UNREADABLE:
                return failure.error + (copy ? " Check that the storage holding your disc copy is still connected, then press A to continue."
                        + " If it stops at the same file again, choose the disc copy again in step 1." + kept
                        : " Check that the storage holding the file is still connected, then press A and choose it again.");
            case NOT_A_BUNDLE:
                return failure.error + " Choose the code bundle file that the build tool on your computer wrote. A: choose again.";
            case BUNDLE_DAMAGED:
                return failure.error + " The file is incomplete or was changed after it was built. Copy it to this device again, then"
                        + " press A and choose it.";
            case BUNDLE_OTHER:
                return failure.error + " Build the bundle again with the build tool that belongs to this app version, from the play"
                        + " disc you copied in step 3.";
            case BUSY:
                return failure.error + " Wait a moment, then press A again.";
            case EXISTING_INSTALL:
                return failure.error + " This page does not replace what is in place. Install the game with " + KIT + ".";
            default:
                return failure.error + " Press A to try again." + kept;
        }
    }

    // ---- free space ----------------------------------------------------------------------------------------------

    /** The free bytes the rest of the copy needs, its reserve included. */
    private long spaceNeed() {
        return disk.bytesMissing + VerifiedExtractor.RESERVE_BYTES;
    }

    /** True when the device has too little room for the files still to copy (asked only while some are). */
    private boolean lowSpace() {
        return spaceNeed() > disk.freeBytes;
    }

    private String spaceSentence() {
        return "It needs " + VerifiedExtractor.gigabytes(spaceNeed(), true) + " of free space; this device has "
                + VerifiedExtractor.gigabytes(disk.freeBytes, false) + " free.";
    }

    /** For the first step, before the player goes looking for the disc: the fact alone (step 3 says what to do). */
    private String lowSpaceSentence() {
        return "This device has too little free space for the copy: " + VerifiedExtractor.gigabytes(spaceNeed(), true)
                + " are needed and " + VerifiedExtractor.gigabytes(disk.freeBytes, false) + " are free (see step 3).";
    }

    /** A need is rounded up and a free amount down, so the text never promises too much; so is what to free. */
    static String lowSpaceText(long need, long free, String then) {
        return "Not enough free space on this device: " + VerifiedExtractor.gigabytes(need, true) + " are needed and "
                + VerifiedExtractor.gigabytes(free, false) + " are free. Free at least "
                + VerifiedExtractor.gigabytes(Math.max(0, need - free), true) + " (remove apps or files you no longer need), " + then + ".";
    }

    private String progressText(String verb) {
        if (bytesTotal <= 0) return verb + "\u2026";
        long session = startBytes < 0 ? 0 : bytesDone - startBytes;
        long seconds = secondsLeft(session, bytesTotal - bytesDone, nowMs - startMs);
        return verb + " " + currentPath + ": " + formatBytes(bytesDone) + " of " + formatBytes(bytesTotal) + ", file "
                + Math.min(filesDone + 1, filesTotal) + " of " + filesTotal + ", " + formatTimeLeft(seconds) + ".";
    }

    /** True when the step's text reports a failure (drawn in the error colour). */
    boolean detailIsProblem(int step) {
        return !readOnly() && state(step) == PROBLEM;
    }

    /** 0..1 while copying or importing, else -1 (no bar). */
    float progressFraction() {
        if (!busy() || working == STEP_PLAY || bytesTotal <= 0) return -1f;
        return Math.max(0f, Math.min(1f, (float) ((double) bytesDone / (double) bytesTotal)));
    }

    /** The button prompts: only what a button does right now is named. */
    String footer() {
        if (overlay == OVERLAY_STOP) return "A Select   B Continue   Left/Right Choose";
        if (overlay == OVERLAY_SOURCE) return "A Select   B Cancel   Left/Right Choose";
        if (busy()) return working == STEP_PLAY || stopping ? "B Back" : "B Stop";
        return actionable(focus) ? "A Select   B Back" : "B Back";
    }

    // ---- small pure helpers ------------------------------------------------------------------------------------

    /**
     * Seconds until the work is done at the speed seen so far, or -1 while that cannot be said yet (less than
     * 2 seconds or less than 16 MiB into this run).
     */
    static long secondsLeft(long sessionBytesDone, long bytesLeft, long elapsedMs) {
        if (bytesLeft <= 0) return 0;
        if (elapsedMs < 2000 || sessionBytesDone < (16L << 20)) return -1;
        double perMs = (double) sessionBytesDone / (double) elapsedMs;
        return (long) Math.ceil(bytesLeft / perMs / 1000.0);
    }

    static String formatTimeLeft(long seconds) {
        if (seconds < 0) return "estimating the time left";
        if (seconds < 60) return "less than a minute left";
        long minutes = (seconds + 59) / 60;
        return "about " + minutes + " min left";
    }

    /** Decimal units as storage is sold: 8.1 GB, 512 MB, 14 KB. */
    static String formatBytes(long bytes) {
        if (bytes >= 1000000000L) return String.format(Locale.ROOT, "%d.%d GB", bytes / 1000000000L, bytes % 1000000000L / 100000000L);
        if (bytes >= 1000000L) return bytes / 1000000L + " MB";
        return bytes / 1000L + " KB";
    }
}
