package dev.recompbootstrap.smoke;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Everything the two-disc setup page decides, without any Android class: the three rows ("1  Play Disc (Disc 2)",
 * "2  Install Disc (Disc 1)", "3  Finish") and their states, the focus, what A / B / Left / Right do, the two
 * questions ("Copy ...?", "Stop copying?") and every text. Pure Java. Used in a build that carries its game code
 * (ShipApplication.carriesGameCode); a developer build keeps SetupModel. SetupDiscScreen draws it through
 * SetupDiscPage and carries out takeAction().
 *
 * The two disc rows work alike and in either order. A chosen image is recognised by Disc2Discs.open; an image of
 * the other disc than the row's is accepted and serves the other row. "Done" on a disc row means every pinned entry
 * of that disc is in place; "Finish" is ready exactly when the ship build's own rule (ShipReady.ready) is.
 */
final class SetupDiscModel {
    static final int PLAY = 0, INSTALL = 1, DISCS = 2;
    static final int ROW_PLAY = 0, ROW_INSTALL = 1, ROW_FINISH = 2, ROWS = 3;
    static final int TODO = 0, READING = 1, COPYING = 2, READY = 3, CONTINUE = 4, DONE = 5, PROBLEM = 6;
    static final int ACT_NONE = 0, ACT_PICK = 1, ACT_CONTINUE = 2, ACT_START = 3, ACT_STOP = 4, ACT_LEAVE = 5, ACT_RECHECK = 6;
    static final int OVERLAY_NONE = 0, OVERLAY_START = 1, OVERLAY_STOP = 2;
    static final int BUTTON_START = 0, BUTTON_NOT_NOW = 1, BUTTON_STOP = 0, BUTTON_GO_ON = 1;

    private static final String[] LABELS = {"1  Play Disc (Disc 2)", "2  Install Disc (Disc 1)", "3  Finish"};
    private static final String[] WORDS = {"To do", "Reading", "Copying", "Ready", "Continue", "Done", "Problem"};
    private static final String[] START_BUTTONS = {"Start", "Not now"}, STOP_BUTTONS = {"Stop", "Continue"};
    private static final String[] DISC_NAME = {"the play disc", "the install disc"};
    private static final String[] DISC_FULL = {"the play disc (Disc 2)", "the install disc (Disc 1)"};
    private static final String[] DISC_OWNER = {"the play disc's", "the install disc's"};

    static final String HEADING_COPY = "Copy", HEADING_STOP = "Stop Copying";
    static final String TEXT_CHECKING = "Looking at what is already on this device.";
    static final String TEXT_CHOOSER = "A: open the system's file chooser. On most devices the file chooser works with the touch screen only.";
    static final String TEXT_READING = "Reading the disc image you chose and checking which disc it is. This takes a moment.";
    static final String TEXT_OPENING = "Opening the disc image you chose before. This takes a moment.";
    static final String TEXT_STOP = "Stop copying? The files that are finished are kept. The file that is being copied now starts again next time.";
    static final String TEXT_READY = "Everything the game needs is on this device: the game is ready. A or B: back to the main menu, then choose Play.";
    /**
     * In place of a row's "A: ..." sentence while another row reads or copies: A does nothing on any row then, so no
     * row's text may offer it.
     */
    static final String TEXT_WAIT = "While the other disc is being read or copied, this row waits.";
    /** What ends a copy besides its own end, said before it starts. Leaving the app stops it as B does (appLeft). */
    static final String TEXT_STOPS = "B stops it at any time, and so does leaving the app; the files that are finished are kept.";
    /**
     * Before a row's text when a copy of its disc was started by ANOTHER process and its end was never seen
     * (SetupDiscStore.lastCopy, LAST_INTERRUPTED): the app was killed or the device restarted in the middle of it. The
     * file in flight never got its name (a file is named only after its check); the files before it were each
     * checked and kept. A copy stopped with B or by leaving the app is not "interrupted": it keeps its own sentences
     * (setup-fixes, check finding M1).
     */
    static final String TEXT_INTERRUPTED = "The last copy was interrupted (the app was closed or the device restarted).";
    static final String TEXT_STOPPED = "Stopped. ", TEXT_STOPPED_LEFT = "The copy stopped because the app was left. ";
    /** After TEXT_INTERRUPTED: what became of the file in flight, true whether or not one had been started. */
    static final String TEXT_NONE_HALF = "Any file that was not finished was removed.";
    static final String TEXT_HALF_LEFT = "The unfinished file is still on this device; it is removed when the copy goes on.";
    /** An image's name longer than this is shortened in the middle on the rows (setup-fixes: names up to 255 characters). */
    static final int NAME_MAX = 60;
    /** The usual size of the image of either disc (a full image as a disc drive reads it; both discs are 8.7 GB). */
    static final long USUAL_IMAGE_BYTES = 8738846720L;

    // ---- what the game needs from the discs --------------------------------------------------------------------------

    /** The pinned entries that come from the discs, by disc. */
    static final class Pins {
        final List<Disc2Discs.Pin> play, install;
        /** Pinned disc entries that are on neither disc's list (none in this release). */
        final List<Disc2Discs.Pin> other;
        /** All of them, the order of the record. */
        final List<Disc2Discs.Pin> all;

        Pins(List<Disc2Discs.Pin> play, List<Disc2Discs.Pin> install, List<Disc2Discs.Pin> other, List<Disc2Discs.Pin> all) {
            this.play = Collections.unmodifiableList(play);
            this.install = Collections.unmodifiableList(install);
            this.other = Collections.unmodifiableList(other);
            this.all = Collections.unmodifiableList(all);
        }

        List<Disc2Discs.Pin> of(int disc) {
            return disc == PLAY ? play : install;
        }

        /**
         * The record's disc entries (ShipConfig.notBundled), told apart by disc with the catalog's two lists
         * (SetupCatalog.items, SetupCatalog.installItems). The record decides what is needed; the catalog only which
         * disc it is on.
         */
        static Pins split(List<Disc2Discs.Pin> fromDiscs, List<SetupCatalog.Item> playItems, List<SetupCatalog.Item> installItems) {
            Set<String> onPlay = new HashSet<String>(), onInstall = new HashSet<String>();
            for (SetupCatalog.Item item : playItems) onPlay.add(item.sha256 + "/" + item.bytes);
            for (SetupCatalog.Item item : installItems) onInstall.add(item.sha256 + "/" + item.bytes);
            List<Disc2Discs.Pin> play = new ArrayList<Disc2Discs.Pin>(), install = new ArrayList<Disc2Discs.Pin>(),
                    other = new ArrayList<Disc2Discs.Pin>(), all = new ArrayList<Disc2Discs.Pin>();
            Set<String> seen = new HashSet<String>();
            for (Disc2Discs.Pin p : fromDiscs) {
                String key = p.sha256 + "/" + p.bytes;
                if (!seen.add(key)) continue;
                all.add(p);
                if (onPlay.contains(key)) play.add(p);
                else if (onInstall.contains(key)) install.add(p);
                else other.add(p);
            }
            return new Pins(play, install, other, all);
        }

    }

    /** What is in place on the device. Gathered by inspect(), which only reads. */
    static final class Disk {
        final Pins pins;
        /** SHA-256 values of the disc entries in place by the ship build's rule. */
        final Set<String> inPlace;
        /** Entries there by name and size that are not the pinned file (ShipData.different): replaced by a copy. */
        final Set<String> notTrusted;
        /** What a copy can count on (left-overs of a killed run included). */
        final long freeBytes;
        /** The remembered choice per disc, or null. */
        final SetupDiscStore.Choice[] remembered;
        /** null when this app version's game code is in place; else the sentence that says what to do. */
        final String codeProblem;
        /** ShipReady.ready(): the rule Play and the launcher use. */
        final boolean ready;
        /** Per disc: SetupDiscStore.lastCopy (LAST_NONE, LAST_INTERRUPTED, LAST_STOPPED, LAST_LEFT). */
        final int[] lastCopy;
        /** Bytes a stopped or killed copy left in the staging folder (removed by the first screen or the next copy). */
        final long leftoverBytes;

        Disk(Pins pins, Set<String> inPlace, Set<String> notTrusted, long freeBytes, SetupDiscStore.Choice[] remembered, String codeProblem,
                boolean ready) {
            this(pins, inPlace, notTrusted, freeBytes, remembered, codeProblem, ready, null, 0);
        }

        Disk(Pins pins, Set<String> inPlace, Set<String> notTrusted, long freeBytes, SetupDiscStore.Choice[] remembered, String codeProblem,
                boolean ready, int[] lastCopy, long leftoverBytes) {
            this.pins = pins;
            this.inPlace = Collections.unmodifiableSet(new HashSet<String>(inPlace));
            this.notTrusted = Collections.unmodifiableSet(new HashSet<String>(notTrusted));
            this.freeBytes = freeBytes;
            this.remembered = remembered == null ? new SetupDiscStore.Choice[DISCS] : remembered.clone();
            this.codeProblem = codeProblem;
            this.ready = ready;
            this.lastCopy = lastCopy == null ? new int[DISCS] : lastCopy.clone();
            this.leftoverBytes = leftoverBytes;
        }

        int have(int disc) {
            return pins.of(disc).size() - missing(disc).size();
        }

        int total(int disc) {
            return pins.of(disc).size();
        }

        List<Disc2Discs.Pin> missing(int disc) {
            return SetupDiscCopy.notIn(pins.of(disc), inPlace);
        }

        long missingBytes(int disc) {
            return SetupDiscCopy.bytes(missing(disc));
        }

        long totalBytes(int disc) {
            return SetupDiscCopy.bytes(pins.of(disc));
        }

        boolean done(int disc) {
            return missing(disc).isEmpty();
        }
    }

    /** The sentence for the Finish row when this app version's game code is not usable; null when it is. */
    static String codeProblem(ShipReady.Stands stands) {
        switch (stands) {
            case NOTHING:
            case DATA:
                return null;
            case APP_FILE:
                return "This app cannot read its own file. " + ShipAdvice.INSTALL_OVER;
            default:
                // PIECE, CODE: one of the pieces the app carries is not in place. The menu's first item then reads
                // "Finish Setting Up" and opens the screen that checks and repairs them.
                return "The app has not finished setting itself up. Go back to the main menu (B) and choose Finish Setting Up.";
        }
    }

    /**
     * Read-only. ship: ShipReady.of(apk, filesDir), the rule Play and the launcher use. Counts follow ShipData.look
     * when the ship rule got that far (a file that is there but is not the pinned one is not in place), else the
     * names and sizes in files/private-trace/inputs.
     */
    static Disk inspect(File filesDir, ShipReady ship, SetupDiscStore store, VerifiedExtractor.Space space) {
        return inspect(filesDir, ship, store, space, SetupCatalog.items(), SetupCatalog.installItems());
    }

    /** As above with the two discs' lists given (tests use synthetic ones). */
    static Disk inspect(File filesDir, ShipReady ship, SetupDiscStore store, VerifiedExtractor.Space space, List<SetupCatalog.Item> playItems,
            List<SetupCatalog.Item> installItems) {
        File root = new File(filesDir, InstallState.PRIVATE_TRACE);
        Pins pins;
        if (ship.config != null) {
            List<Disc2Discs.Pin> fromDiscs = new ArrayList<Disc2Discs.Pin>();
            for (ShipConfig.DataFile file : ship.config.notBundled) fromDiscs.add(new Disc2Discs.Pin(file.sha256, file.bytes));
            pins = Pins.split(fromDiscs, playItems, installItems);
        } else {
            List<Disc2Discs.Pin> listed = new ArrayList<Disc2Discs.Pin>();
            for (SetupCatalog.Item item : playItems) listed.add(new Disc2Discs.Pin(item.sha256, item.bytes));
            for (SetupCatalog.Item item : installItems) listed.add(new Disc2Discs.Pin(item.sha256, item.bytes));
            pins = Pins.split(listed, playItems, installItems);
        }
        Set<String> inPlace = new HashSet<String>(), notTrusted = new HashSet<String>();
        if (ship.data != null) {
            Set<String> out = new HashSet<String>();
            for (ShipConfig.DataFile file : ship.data.absent) out.add(file.sha256);
            for (ShipConfig.DataFile file : ship.data.different) {
                out.add(file.sha256);
                notTrusted.add(file.sha256);
            }
            for (Disc2Discs.Pin p : pins.all) {
                if (!out.contains(p.sha256)) inPlace.add(p.sha256);
            }
        } else {
            inPlace.addAll(Disc2Discs.installed(new File(root, SetupDiscCopy.INPUTS), pins.all));
        }
        SetupDiscStore.Choice[] remembered = {store.load(PLAY), store.load(INSTALL)};
        // "Ready" is asked exactly as the menu's Play tab asks it (LauncherScreen.detect): the ship build's own rule
        // narrowed by the launcher's look at the start request, so that Finish and Play can never disagree.
        int[] lastCopy = {store.lastCopy(PLAY), store.lastCopy(INSTALL)};
        return new Disk(pins, inPlace, notTrusted, SetupDiscCopy.freeWithLeftovers(root, space), remembered, codeProblem(ship.stands),
                ship.ready() && InstallState.detect(filesDir, ship).ready(), lastCopy, SetupDiscCopy.leftoverBytes(root));
    }

    /** What an image turned out to be, for the page. */
    static final class Recognition {
        /** PLAY or INSTALL when usable, else -1. */
        final int disc;
        final Disc2Discs.Kind kind;
        final String message, next;
        /** For a usable disc: how many of its entries it holds no file of the right size for (Plan.shortSizes). */
        final int lacking;
        /** The image's size in bytes, or -1. */
        final long imageBytes;

        Recognition(int disc, Disc2Discs.Kind kind, String message, String next, int lacking, long imageBytes) {
            this.disc = disc;
            this.kind = kind;
            this.message = message;
            this.next = next;
            this.lacking = lacking;
            this.imageBytes = imageBytes;
        }
    }

    static Recognition recognise(Disc2Discs.Disc found, Pins pins, Set<String> inPlace, long imageBytes) {
        int disc = found.kind == Disc2Discs.Kind.PLAY ? PLAY : found.kind == Disc2Discs.Kind.INSTALL ? INSTALL : -1;
        int lacking = 0;
        if (disc >= 0) lacking = Disc2Discs.plan(found.candidates, pins.of(disc), inPlace).shortSizes.size();
        return new Recognition(disc, found.kind, found.message, found.next, lacking, imageBytes);
    }

    /** What a row expects to be chosen: "Choose the image file of the play disc (Disc 2), usually an .iso file of about 8.7 GB." */
    static String expectedImage(int row) {
        return "Choose the image file of " + DISC_FULL[row] + ", usually an .iso file of about " + SetupModel.formatBytes(USUAL_IMAGE_BYTES) + ".";
    }

    /**
     * Whether the read permission taken for a chosen document is given back at once: when the document was not
     * recognised as one of the two discs (nothing is remembered for it, so nothing would ever give it back; Android
     * keeps a limited number of them per app). permission: it was taken. (setup-fixes, defect C4-4.)
     */
    static boolean releaseGrant(boolean permission, int recognisedDisc) {
        return permission && recognisedDisc < 0;
    }

    // ---- the page's own state -----------------------------------------------------------------------------------------

    private static final int IDLE = 0, P_READING = 1, RECOGNISED = 2, P_COPYING = 3, STOPPED = 4, FAILED = 5;

    private Disk disk = new Disk(new Pins(new ArrayList<Disc2Discs.Pin>(), new ArrayList<Disc2Discs.Pin>(), new ArrayList<Disc2Discs.Pin>(),
            new ArrayList<Disc2Discs.Pin>()), new HashSet<String>(), new HashSet<String>(), 0, null, null, false);
    private boolean inspected;
    /** The look that holds the gate (lookStarts), or null; and whether another look was asked for while it ran. */
    private Look looking;
    private boolean lookAgain;
    private int focus;
    private int overlay = OVERLAY_NONE, overlayFocus;
    private int overlayDisc = -1;
    private int pending = ACT_NONE, pendingDisc = -1;
    private final int[] phase = {IDLE, IDLE};
    /** Per row: what went wrong (a refused image or a failed copy), shown with the next step. */
    private final String[] problem = new String[DISCS];
    /** Per disc: the failed copy's kind, to choose what A does (continue or choose again). */
    private final VerifiedExtractor.Problem[] failure = new VerifiedExtractor.Problem[DISCS];
    /** Per disc: its copy stopped because the app left the front; the row says so until its next copy. */
    private final boolean[] stoppedByLeaving = new boolean[DISCS];
    /** Per disc: "you chose the install disc on the play disc's row" and the like. */
    private final String[] note = new String[DISCS];
    /** Per disc: the name and size of the last image used, to say which file may be deleted afterwards. */
    private final String[] imageName = new String[DISCS];
    private final long[] imageBytes = {-1, -1};
    private boolean continuing;
    private int nothingChosen = -1;
    private int copying = -1;
    private boolean stopping;
    /** The copy was told to stop because the app left the front (appLeft), not by the player's B. */
    private boolean leftApp;
    /**
     * Per disc: an image of it was read in this run of the page, so the screen still holds it and A can go on with
     * it also when the image's provider gave no lasting permission (then nothing is remembered on storage).
     */
    private final boolean[] heldNow = new boolean[DISCS];
    private long bytesDone, bytesTotal, startMs, nowMs, startBytes = -1;
    private int fileIndex, files;
    private String currentName = "";

    /** Takes what inspect() found. The first time, the focus goes to the first row that still needs the player. */
    void setDisk(Disk found) {
        disk = found;
        for (int d = 0; d < DISCS; ++d) {
            if (found.remembered[d] != null && found.remembered[d].name != null && imageName[d] == null) {
                imageName[d] = found.remembered[d].name;
                imageBytes[d] = found.remembered[d].bytes;
            }
        }
        if (!inspected) {
            inspected = true;
            focus = firstOpenRow();
        }
    }

    /** A disc whose copy can be continued first, then the first disc not done, then Finish. */
    private int firstOpenRow() {
        for (int d = 0; d < DISCS; ++d) {
            if (!disk.done(d) && (disk.remembered[d] != null || phase[d] == STOPPED)) return d;
        }
        for (int d = 0; d < DISCS; ++d) {
            if (!disk.done(d)) return d;
        }
        return ROW_FINISH;
    }

    Disk disk() {
        return disk;
    }

    /** One look at the files and the free space (about 74 MB are read): the token of the gate. Made only by lookStarts. */
    static final class Look {
        private Look() {}
    }

    /**
     * A look at the files and the free space may start now: nothing is read or copied and no look runs. Returns the
     * look's token (the screen's inspect takes it, so no look can start without the gate), or null. The screen asks it
     * when the page is shown, when the app or its window comes back, on A, and after a remembered image was forgotten;
     * its twin in the tests asks the same (setup-fixes, C3-1, checks M2 and minor 3).
     */
    Look lookStarts() {
        if (busy() || looking != null) return null;
        looking = new Look();
        return looking;
    }

    /** No look could start now, but one is wanted (the state changed meanwhile): it runs when the running look ends. */
    void lookLater() {
        lookAgain = true;
    }

    /**
     * The look with this token ended. Only the look that took the gate releases it; another token changes nothing.
     * Returns true when a look was asked for meanwhile (lookLater): the caller starts it with lookStarts().
     */
    boolean lookDone(Look look) {
        if (look == null || look != looking) return false;
        looking = null;
        boolean again = lookAgain;
        lookAgain = false;
        return again;
    }

    boolean busy() {
        return copying >= 0 || phase[PLAY] == P_READING || phase[INSTALL] == P_READING;
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

    /** The disc the last action is for (ACT_PICK: the row that asks; others: the disc). */
    int actionDisc() {
        return pendingDisc;
    }

    int takeAction() {
        int action = pending;
        pending = ACT_NONE;
        return action;
    }

    private void act(int action, int disc) {
        pending = action;
        pendingDisc = disc;
    }

    /** The word over the open question: what the question is about, instead of the frame's "Alert". "" without a question. */
    String overlayHeading() {
        return overlay == OVERLAY_START ? HEADING_COPY : overlay == OVERLAY_STOP ? HEADING_STOP : "";
    }

    String overlayText() {
        if (overlay == OVERLAY_STOP) return TEXT_STOP;
        if (overlay != OVERLAY_START) return "";
        int d = overlayDisc;
        List<Disc2Discs.Pin> missing = disk.missing(d);
        return (note[d] == null ? "" : note[d] + " ") + "Copy " + DISC_OWNER[d] + " files to this device? " + files(missing.size()) + ", "
                + SetupModel.formatBytes(SetupDiscCopy.bytes(missing)) + ". " + spaceSentence(d) + " " + TEXT_STOPS;
    }

    String[] overlayButtons() {
        if (overlay == OVERLAY_START) return START_BUTTONS.clone();
        if (overlay == OVERLAY_STOP) return STOP_BUTTONS.clone();
        return new String[0];
    }

    /** Touch: a button of the open question was tapped. */
    void overlayTap(int button) {
        if (overlay == OVERLAY_NONE || button < 0 || button >= overlayButtons().length) return;
        overlayFocus = button;
        activateOverlay();
    }

    private void activateOverlay() {
        int kind = overlay;
        overlay = OVERLAY_NONE;
        if (kind == OVERLAY_STOP) {
            if (overlayFocus == BUTTON_STOP && copying >= 0) {
                stopping = true;
                act(ACT_STOP, copying);
            }
        } else if (kind == OVERLAY_START && overlayFocus == BUTTON_START && !busy() && phase[overlayDisc] == RECOGNISED) {
            act(ACT_START, overlayDisc);
        }
    }

    /** Touch: put the focus on a row. */
    void focus(int row) {
        if (overlay == OVERLAY_NONE && row >= 0 && row < ROWS) focus = row;
    }

    /** A NavInput command. Returns true if handled; false for BACK means "leave this page". */
    boolean nav(int command, boolean repeat) {
        if (overlay != OVERLAY_NONE) {
            if (command == NavInput.LEFT) overlayFocus = Math.max(0, overlayFocus - 1);
            else if (command == NavInput.RIGHT) overlayFocus = Math.min(overlayButtons().length - 1, overlayFocus + 1);
            else if (command == NavInput.BACK && !repeat) overlay = OVERLAY_NONE;
            else if (command == NavInput.CONFIRM && !repeat) activateOverlay();
            return true;
        }
        switch (command) {
            case NavInput.UP:
                if (focus > 0) --focus; else if (!repeat) focus = ROWS - 1;
                return true;
            case NavInput.DOWN:
                if (focus < ROWS - 1) ++focus; else if (!repeat) focus = 0;
                return true;
            case NavInput.CONFIRM:
                if (!repeat) confirm();
                return true;
            case NavInput.BACK:
                if (repeat) return true;
                // Reading an image writes nothing, and a copy that was told to stop only winds down: the page can be
                // left, so that a source that never answers cannot hold the player here.
                if (copying < 0 || stopping) return false;
                overlay = OVERLAY_STOP;
                overlayFocus = BUTTON_STOP;   // the player pressed B to leave, so A confirms that
                return true;
            default:
                return false;
        }
    }

    private void confirm() {
        if (!inspected || busy()) return;
        if (focus == ROW_FINISH) {
            if (finishReady()) act(ACT_LEAVE, -1);
            return;
        }
        int d = focus;
        switch (state(d)) {
            case READY:
                overlay = OVERLAY_START;
                overlayDisc = d;
                overlayFocus = BUTTON_START;
                break;
            case CONTINUE:
                act(ACT_CONTINUE, d);
                break;
            case PROBLEM:
                if (phase[d] == RECOGNISED) act(ACT_RECHECK, d);   // too little space: look again after the player made room
                else if (phase[d] == FAILED && failure[d] != null && resumable(failure[d]) && canGoOn(d)) act(ACT_CONTINUE, d);
                else act(ACT_PICK, d);
                break;
            case TODO:
                act(ACT_PICK, d);
                break;
            default:
                break;
        }
    }

    /** A failed copy that may work when continued with the same image. */
    private static boolean resumable(VerifiedExtractor.Problem p) {
        return p != VerifiedExtractor.Problem.DISC_COPY_BAD;
    }

    /**
     * True when A can go on with the image of this disc without the file chooser: its choice is remembered on storage
     * (the provider gave a lasting permission), or the image was read in this run of the page and the screen still
     * holds it. The row's text says "press A to continue" exactly then.
     */
    private boolean canGoOn(int d) {
        return disk.remembered[d] != null || heldNow[d];
    }

    // ---- what the screen reports back --------------------------------------------------------------------------------

    /** The file chooser of a row was closed without a choice, or none could be opened. */
    void nothingChosen(int row) {
        if (!busy()) nothingChosen = row;
    }

    /** An image is being read for row (a new choice, or continuing: the remembered one). */
    void readingStarted(int row, boolean fromMemory) {
        phase[row] = P_READING;
        continuing = fromMemory;
        nothingChosen = -1;
        problem[row] = null;
        failure[row] = null;
    }

    /** The remembered image of row could not be opened: it is gone, or the permission to read it is. */
    void rememberedGone(int row, String reason) {
        phase[row] = FAILED;
        failure[row] = null;
        continuing = false;
        problem[row] = "The disc image you chose before can no longer be read (" + reason + "). It may have been moved, renamed or deleted,"
                + " or the storage it is on was removed. A: choose it again.";
    }

    /** What the chosen image is. name: its name for the player (or null). After setDisk with a fresh inspection. */
    void imageRead(int row, Recognition r, String name) {
        boolean fromMemory = continuing;
        continuing = false;
        phase[row] = IDLE;
        if (r.disc < 0) {
            phase[row] = FAILED;
            failure[row] = null;
            // Not a disc image at all: the message says what the file is not (and what it is, when that is known); the
            // next sentence is the one thing that is expected, the image of this row's disc (setup-fixes, C4-2).
            String next = r.kind == Disc2Discs.Kind.NOT_A_DISC ? expectedImage(row) : r.next;
            problem[row] = (fromMemory ? "The disc image you chose before is not usable now. " : "") + r.message
                    + (next == null ? "" : " " + next) + " A: choose again.";
            return;
        }
        int d = r.disc;
        note[d] = null;
        if (d != row) {
            note[d] = "You chose " + DISC_FULL[d] + " on the row of " + DISC_FULL[row] + ". That is fine: it is used for row " + (d + 1) + ".";
            note[row] = null;
            focus = d;
            if (phase[row] == FAILED) phase[row] = IDLE;
        }
        imageName[d] = name;
        imageBytes[d] = r.imageBytes;
        heldNow[d] = true;
        problem[d] = null;
        failure[d] = null;
        if (disk.done(d)) {
            phase[d] = IDLE;
            return;
        }
        if (r.lacking > 0) {
            phase[d] = FAILED;
            problem[d] = r.message + " But this image lacks " + r.lacking + " of the " + disk.total(d) + " files the game needs from "
                    + DISC_NAME[d] + ": it is incomplete or damaged. " + Disc2Discs.NEXT_NEW_COPY + " Then press A and choose it.";
            return;
        }
        phase[d] = RECOGNISED;
        if (lowSpace(d)) return;
        if (fromMemory) {
            act(ACT_START, d);   // the player pressed Continue: the copy goes on without a second question
            return;
        }
        overlay = OVERLAY_START;
        overlayDisc = d;
        overlayFocus = BUTTON_START;
    }

    void copyStarted(int disc, long now) {
        copying = disc;
        phase[disc] = P_COPYING;
        stopping = false;
        leftApp = false;
        startMs = now;
        nowMs = now;
        startBytes = -1;
        bytesDone = bytesTotal = 0;
        fileIndex = files = 0;
        currentName = "";
        problem[disc] = null;
        failure[disc] = null;
    }

    void progress(long done, long total, int index, int count, String name, long now) {
        if (copying < 0) return;
        if (startBytes < 0 && total > 0) startBytes = done;
        bytesDone = done;
        bytesTotal = total;
        fileIndex = index;
        files = count;
        currentName = name == null ? "" : name;
        nowMs = now;
    }

    /**
     * The app left the front (Home, another app, the screen switched off) while this page is shown. A running copy
     * is told to stop, exactly as the player's B and "Stop" do: returns true when the screen must now stop the copy
     * (it sets the same flag B's ACT_STOP sets). The copy then ends at its next block: the file in flight is removed,
     * every finished file stays (each was checked and forced to storage before it got its name), and the row offers
     * Continue. Why it stops instead of going on: a copy that went on would be left to whatever Android does with an
     * app that is not in front (go on, pause, end it) with up to one disc file half written; stopping leaves nothing
     * in flight and nothing half written, on every device, and the page then says what happened. An open "Stop
     * copying?" question is closed: it was answered.
     */
    boolean appLeft() {
        return appLeft(null);
    }

    /**
     * As appLeft(), and the running copy's marker is told first (it then records "left", not "stopped"), before the
     * screen sets the copy's cancel flag: the copy thread that sees the flag also sees the marker's note (both
     * volatile, written in this order). A copy already stopping (B, then Home) is not told: it stays "stopped".
     * (setup-fixes, check-2 minors 1 and 2.)
     */
    boolean appLeft(SetupDiscStore.Marker marker) {
        if (copying < 0 || stopping) return false;
        if (marker != null) marker.appLeft();
        stopping = true;
        leftApp = true;
        if (overlay == OVERLAY_STOP) overlay = OVERLAY_NONE;
        return true;
    }

    /** Call after setDisk() with a fresh inspection, so that Done reflects what is really in place. */
    void copyFinished(SetupDiscCopy.Outcome outcome) {
        int d = copying;
        boolean left = leftApp;
        copying = -1;
        stopping = false;
        leftApp = false;
        overlay = OVERLAY_NONE;
        if (d < 0) return;
        note[d] = null;
        stoppedByLeaving[d] = false;
        if (disk.done(d)) {
            phase[d] = IDLE;
            problem[d] = null;
            failure[d] = null;
            if (focus == d) focus = firstOpenRow();
            return;
        }
        if (outcome.stopped && outcome.error == null) {
            phase[d] = STOPPED;
            stoppedByLeaving[d] = left;
            return;
        }
        phase[d] = FAILED;
        failure[d] = outcome.problem;
        problem[d] = failureText(d, outcome);
    }

    // ---- what the screen draws ------------------------------------------------------------------------------------

    String title() {
        return "Set up game files";
    }

    String label(int row) {
        return LABELS[row];
    }

    boolean finishReady() {
        return disk.ready && disk.codeProblem == null;
    }

    int state(int row) {
        if (!inspected) return TODO;
        if (row == ROW_FINISH) return finishReady() ? DONE : disk.codeProblem != null ? PROBLEM : TODO;
        int d = row;
        if (phase[d] == P_READING) return READING;
        if (phase[d] == P_COPYING) return COPYING;
        if (disk.done(d)) return DONE;
        if (phase[d] == FAILED) return PROBLEM;
        if (phase[d] == RECOGNISED) return lowSpace(d) ? PROBLEM : READY;
        if (phase[d] == STOPPED || disk.remembered[d] != null) return CONTINUE;
        return TODO;
    }

    String stateWord(int row) {
        return WORDS[state(row)];
    }

    /** True when A on the row would do something now (the row is drawn enabled). */
    boolean actionable(int row) {
        if (!inspected || busy()) return false;
        if (row == ROW_FINISH) return finishReady();
        int s = state(row);
        return s == TODO || s == READY || s == CONTINUE || s == PROBLEM;
    }

    boolean detailIsProblem(int row) {
        return state(row) == PROBLEM;
    }

    String detail(int row) {
        if (!inspected) return TEXT_CHECKING;
        if (row == ROW_FINISH) return finishText();
        int d = row;
        String noted = note[d] == null ? "" : note[d] + " ";
        switch (state(d)) {
            case READING:
                return continuing ? TEXT_OPENING : TEXT_READING;
            case COPYING:
                return stopping ? "Stopping\u2026" : progressText(d);
            case DONE:
                return noted + "All " + disk.total(d) + " files the game needs from " + DISC_NAME[d] + " are on this device ("
                        + SetupModel.formatBytes(disk.totalBytes(d)) + ")." + deleteSentence(d);
            case PROBLEM: {
                String text;
                if (phase[d] == FAILED && failure[d] == VerifiedExtractor.Problem.NO_SPACE) {
                    text = noSpaceText(d);   // the figures of the last look, not those of the moment the copy failed
                } else if (phase[d] == RECOGNISED) {
                    text = noted + "Recognised: " + DISC_FULL[d] + ". " + SetupModel.lowSpaceText(spaceNeed(d), disk.freeBytes,
                            "then press A to look again");
                } else {
                    text = problem[d] == null ? "" : problem[d];
                }
                // These texts name A in their own sentence; while another row works, A does nothing here either.
                return busy() && !text.isEmpty() ? text + " " + TEXT_WAIT : text;
            }
            case READY:
                return noted + "Recognised: " + DISC_FULL[d] + ". The copy takes " + files(disk.missing(d).size()) + ", "
                        + SetupModel.formatBytes(disk.missingBytes(d)) + ". " + spaceSentence(d) + " " + offer("A: start the copy.");
            case CONTINUE: {
                String which = imageName[d] == null ? "the disc image you chose" : shortName(imageName[d]);
                String interrupted = phase[d] != STOPPED ? interruptedText(d) : "";
                if (!interrupted.isEmpty()) {
                    return interrupted + "The copy goes on from " + which + ". " + spaceSentence(d) + " " + offer("A: continue.");
                }
                String stopped = stoppedText(d);
                return stopped + disk.have(d) + " of " + disk.total(d) + " files of " + DISC_NAME[d]
                        + " " + isAre(disk.have(d)) + " on this device. The copy can go on from " + which + " where it stopped; the file that was being copied"
                        + " starts again. " + spaceSentence(d) + " " + offer("A: continue.");
            }
            default:
                return todoText(d);
        }
    }

    /** A row's sentence about what A does: said only while A would do it (not while another row reads or copies). */
    private String offer(String whenFree) {
        return busy() ? TEXT_WAIT : whenFree;
    }

    private String todoText(int d) {
        String start;
        if (nothingChosen == d) {
            start = "Nothing was chosen. If no file chooser opened, this device has none, and the game's files cannot be set up on it. ";
        } else if (!interruptedText(d).isEmpty()) {
            start = interruptedText(d) + "To go on, choose " + DISC_NAME[d] + "'s disc image again. ";
        } else if (disk.have(d) > 0) {
            start = stoppedText(d) + disk.have(d) + " of " + disk.total(d) + " files of " + DISC_NAME[d] + " " + isAre(disk.have(d)) + " already on this device. To copy the rest,"
                    + " choose its disc image again. ";
        } else {
            start = cap(DISC_FULL[d]) + ": choose its disc image (an .iso file) on this device. The game needs " + files(disk.total(d))
                    + " from it, " + SetupModel.formatBytes(disk.totalBytes(d)) + "; each one is checked as it is copied. ";
        }
        String space = lowSpace(d) ? SetupModel.lowSpaceText(spaceNeed(d), disk.freeBytes, "before you start") + " " : "";
        return start + space + offer(TEXT_CHOOSER);
    }

    /**
     * "" unless a copy of disc d was cut off in an earlier run of the app; else what happened and what is kept, as
     * sentences that end with a space; the caller adds that the unfinished file was removed and what A does
     * (setup-fixes, C1-1). Short: the row must be drawn whole with a long image name (SetupDiscPageTest.expectFits).
     */
    private String interruptedText(int d) {
        if (disk.lastCopy[d] != SetupDiscStore.LAST_INTERRUPTED || disk.done(d) || phase[d] == STOPPED) return "";
        int have = disk.have(d), total = disk.total(d);
        String kept = have == 0 ? "No file of " + DISC_NAME[d] + " had been finished yet."
                : have == 1 ? "1 of the " + total + " files is finished and kept; it was checked when it was copied."
                : have + " of the " + total + " files are finished and kept; each was checked when it was copied.";
        // Said only as it is: a half file still lies there when the app's first screen did not remove it.
        return TEXT_INTERRUPTED + " " + kept + " " + (disk.leftoverBytes > 0 ? TEXT_HALF_LEFT : TEXT_NONE_HALF) + " ";
    }

    /**
     * "" or who stopped the last copy of disc d: this page saw it (phase STOPPED), or the marker says so (a new page in
     * the same process after B to the menu, or a later run of the app): B, or the app leaving the front.
     */
    private String stoppedText(int d) {
        if (disk.done(d)) return "";
        if (phase[d] == STOPPED) return stoppedByLeaving[d] ? TEXT_STOPPED_LEFT : TEXT_STOPPED;
        if (phase[d] != IDLE) return "";
        return disk.lastCopy[d] == SetupDiscStore.LAST_LEFT ? TEXT_STOPPED_LEFT : disk.lastCopy[d] == SetupDiscStore.LAST_STOPPED ? TEXT_STOPPED : "";
    }

    /** An image's name as the rows show it: whole up to NAME_MAX characters, else its start and its end. */
    static String shortName(String name) {
        if (name == null || name.length() <= NAME_MAX) return name;
        int keep = (NAME_MAX - 1) / 2;
        return name.substring(0, keep) + "\u2026" + name.substring(name.length() - keep);
    }

    private String finishText() {
        if (disk.codeProblem != null) return disk.codeProblem;
        if (finishReady()) return TEXT_READY;
        StringBuilder out = new StringBuilder();
        for (int d = 0; d < DISCS; ++d) {
            List<Disc2Discs.Pin> missing = disk.missing(d);
            if (missing.isEmpty()) continue;
            String amount = files(missing.size()) + ", " + SetupModel.formatBytes(SetupDiscCopy.bytes(missing));
            if (missing.size() == disk.total(d)) out.append(cap(DISC_OWNER[d])).append(" files are still missing: ").append(amount).append(". ");
            else out.append("Some of ").append(DISC_OWNER[d]).append(" files are still missing: ").append(amount).append(" of ")
                    .append(disk.total(d)).append(". ");
        }
        List<Disc2Discs.Pin> other = SetupDiscCopy.notIn(disk.pins.other, disk.inPlace);
        if (!other.isEmpty()) {
            out.append(other.size() == 1 ? "1 other game file is" : other.size() + " other game files are").append(" still missing (")
                    .append(SetupModel.formatBytes(SetupDiscCopy.bytes(other))).append("); they are on neither disc's list. ");
        }
        if (out.length() == 0) {
            // Every disc file is in place by this page's count, but the ship rule does not call the game ready yet.
            // The menu's first item then reads "Finish Setting Up" (one of the app's own pieces or folders is not in place).
            return "The disc files are on this device, but the app has not finished setting itself up. Go back to the main menu (B) and"
                    + " choose Finish Setting Up.";
        }
        long whole = disk.totalBytes(PLAY) + disk.totalBytes(INSTALL) + SetupDiscCopy.bytes(disk.pins.other);
        out.append("The whole game needs about ").append(VerifiedExtractor.gigabytes(whole, true))
                .append(" of space on this device, plus room for one disc image at a time while it is copied.");
        return out.toString();
    }

    /** After a disc is done: its image may go. The app never deletes a file of the player. */
    private String deleteSentence(int d) {
        String name = imageName[d];
        if (name == null) {
            return " If you put its disc image on this device for this, you may now delete it to free space: this app does not need it again"
                    + " and never deletes your files.";
        }
        String size = imageBytes[d] > 0 ? " to free " + SetupModel.formatBytes(imageBytes[d]) : " to free space";
        return " You may now delete the image file " + shortName(name) + size + ": this app does not need it again and never deletes your files.";
    }

    private String failureText(int d, SetupDiscCopy.Outcome failure) {
        int have = disk.have(d);
        String kept = have == 0 ? "" : have == 1 ? " The 1 file already copied is kept." : " The " + have + " files already copied are kept.";
        // The page's own sentence of the failure (Outcome.shown): "file k of n", never the name of a file of the disc.
        // The log's sentence (Outcome.error) keeps the name and the reason.
        String error = failure.shown == null ? "The copy ended without a result." : failure.shown;
        switch (failure.problem) {
            case NO_SPACE:
                return noSpaceText(d);
            case DISC_COPY_BAD:
                boolean many = error.endsWith(SetupDiscCopy.SHOWN_DAMAGED_MANY);
                if (many || error.endsWith(SetupDiscCopy.SHOWN_DAMAGED)) {
                    // A file was read to its end and failed its check (setup-fixes, C5-1): say which, and that the
                    // others stay; only the missing ones are copied again.
                    // Both possibilities, as for any bad copy: a damaged file, or another release's file of that size.
                    return error.substring(0, error.length() - 1) + " (or this image is not a copy of the supported disc): "
                            + (many ? "they did not pass their check and were not kept." : "it did not pass its check and was not kept.") + kept
                            + " " + Disc2Discs.NEXT_NEW_COPY + " Then press A and choose it; only the missing files are copied.";
                }
                return error + " This disc image is damaged or is not a copy of the supported disc. " + Disc2Discs.NEXT_NEW_COPY
                        + " Then press A and choose it." + kept;
            case SOURCE_UNREADABLE:
                return error + " Check that the storage the disc image is on is still connected, then press A to continue." + kept;
            case BUSY:
                return error + " Wait a moment, then press A again.";
            default:
                return error + " Press A to try again." + kept;
        }
    }

    /**
     * The row of a copy that stopped for lack of space, built from the figures of the last look (inspect runs when the
     * page is shown, when the app or its window comes back to the front, and on A), never from those of the moment it
     * failed (setup-fixes, C3-1).
     */
    private String noSpaceText(int d) {
        int have = disk.have(d);
        String kept = have == 0 ? "" : have == 1 ? " The 1 file already copied is kept." : " The " + have + " files already copied are kept.";
        if (!lowSpace(d)) return "The copy stopped for lack of free space. There is enough now. " + spaceSentence(d) + " A: continue." + kept;
        return "The copy stopped. " + SetupModel.lowSpaceText(spaceNeed(d), disk.freeBytes, "then press A to continue") + kept;
    }

    // ---- free space ---------------------------------------------------------------------------------------------------

    /** The free bytes the rest of this disc's copy needs, its reserve included. */
    long spaceNeed(int d) {
        return disk.missingBytes(d) + SetupDiscCopy.RESERVE_BYTES;
    }

    boolean lowSpace(int d) {
        return !disk.done(d) && spaceNeed(d) > disk.freeBytes;
    }

    private String spaceSentence(int d) {
        return "It needs " + VerifiedExtractor.gigabytes(spaceNeed(d), true) + " of free space (1 GB of it stays free for the system);"
                + " this device has " + VerifiedExtractor.gigabytes(disk.freeBytes, false) + " free.";
    }

    // ---- progress -------------------------------------------------------------------------------------------------------

    private String progressText(int d) {
        if (bytesTotal <= 0) return "Copying " + DISC_OWNER[d] + " files\u2026";
        long session = startBytes < 0 ? 0 : bytesDone - startBytes;
        long seconds = SetupModel.secondsLeft(session, bytesTotal - bytesDone, nowMs - startMs);
        // "file 3 of 10" with the amounts. The file's own name on the disc is an internal name and is not shown
        // (the lead's decision of 2026-10-06); the log's FILE lines keep it.
        return "Copying " + DISC_OWNER[d] + " files: file " + Math.min(fileIndex + 1, files) + " of " + files + ". "
                + SetupModel.formatBytes(bytesDone) + " of " + SetupModel.formatBytes(bytesTotal) + ", "
                + SetupModel.formatTimeLeft(seconds) + ". B: stop.";
    }

    /** 0..1 while copying, else -1 (no bar). */
    float progressFraction() {
        if (copying < 0 || bytesTotal <= 0) return -1f;
        return Math.max(0f, Math.min(1f, (float) ((double) bytesDone / (double) bytesTotal)));
    }

    /** The row that is copying, or -1. */
    int copyingRow() {
        return copying;
    }

    String footer() {
        if (overlay != OVERLAY_NONE) return "A Select   B " + (overlay == OVERLAY_STOP ? "Continue" : "Cancel") + "   Left/Right Choose";
        if (copying >= 0) return stopping ? "B Back" : "B Stop";
        if (busy()) return "B Back";
        return actionable(focus) ? "A Select   B Back" : "B Back";
    }

    // ---- small helpers -------------------------------------------------------------------------------------------------

    private static String isAre(int n) {
        return n == 1 ? "is" : "are";
    }

    private static String files(int n) {
        return n == 1 ? "1 file" : n + " files";
    }

    private static String cap(String text) {
        return text.isEmpty() ? text : Character.toUpperCase(text.charAt(0)) + text.substring(1);
    }
}
