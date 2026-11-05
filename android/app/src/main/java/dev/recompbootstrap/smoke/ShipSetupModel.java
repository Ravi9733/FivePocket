package dev.recompbootstrap.smoke;

/**
 * Everything the first screen of the build that is handed out decides and says, without any Android class. Pure
 * Java. ShipStartActivity draws heading(), text(), fraction() and the one button, reports what the work did, and
 * carries out the action of the button that was pressed.
 *
 * The screen is "Setting up": the app puts its own pieces in place (the game's own code, its graphics files and
 * the small game files it carries; ShipFirstRun), shows how far that is, and is gone when it has finished. It asks
 * nothing and offers no folder: the game data from the player's two discs is copied later, from the menu's "Set Up
 * Game Files" (the two-disc setup page). When a piece cannot be put in place the screen says so in plain words and
 * offers "Try again"; B or Android's Back closes it.
 */
final class ShipSetupModel {
    enum Phase {
        /** Looking at what is installed. */
        CHECKING,
        /** ShipFirstRun is working. */
        PREPARING,
        /** ShipFirstRun failed, or the app's own file could not be read; text() says it. */
        PREPARE_FAILED,
        /** Nothing (more) to do here: the menu follows. */
        FINISHED
    }

    static final int ACT_NONE = 0, ACT_RETRY = 1, ACT_CLOSE = 2;
    static final String HEADING = "Setting up", HEADING_FAILED = "Setting up did not finish";
    static final String TRY_AGAIN = "Try again";
    static final String ONCE = " This happens once after an install or an update. Keep the app open.";
    static final String CHECKING = "Checking what is already installed.";
    /** Under a failure: what the two buttons do. */
    static final String WAY_OUT = "A: try again. B: close the app.", WAY_BACK = "A: try again. B: back to the menu.";

    private Phase phase = Phase.CHECKING;
    private String message = "";
    /** True when the screen was opened from the menu ("Finish Setting Up"): B then goes back to the menu. */
    private boolean fromMenu;
    private ShipFirstRun.Step step = ShipFirstRun.Step.RECORD;
    private long stepDone, stepTotal;

    Phase phase() {
        return phase;
    }

    void opened(boolean openedFromMenu) {
        fromMenu = openedFromMenu;
    }

    // ---- what the screen reports ---------------------------------------------------------------------------

    void prepareStarted() {
        phase = Phase.PREPARING;
        step = ShipFirstRun.Step.RECORD;
        stepDone = stepTotal = 0;
    }

    void prepareProgress(ShipFirstRun.Step at, long done, long total) {
        if (phase != Phase.PREPARING || at == null) return;
        step = at;
        stepDone = done;
        stepTotal = total;
    }

    /**
     * The first-run work failed, or the app's own file could not be read. error: the work's own sentence
     * (ShipFirstRun's, ShipStart's), which the log keeps whole; the screen shows plainFailure(error).
     */
    void prepareFailed(String error) {
        phase = Phase.PREPARE_FAILED;
        message = plainFailure(error);
    }

    static final String FAILED_APP_FILE = "This app's file is damaged or incomplete. " + ShipAdvice.INSTALL_OVER;
    static final String FAILED_OTHER_USER = "This app only works for the main user of this device. Open it as that user.";
    static final String FAILED_WRITING = "The app could not put its files in place. Check that this device has free storage, then try"
            + " again. If it happens again, " + ShipAdvice.INSTALL_OVER_LOWER;
    /**
     * A file stands where one of the app's folders belongs. Free storage is not the cause, trying again changes nothing
     * and neither does installing over the app; the one thing that does is said with what it costs.
     */
    static final String FAILED_IN_THE_WAY = "The app could not put its files in place: something else stands where one of its"
            + " folders belongs. Installing over this app does not change that. Only removing the app does, and removing it"
            + " also removes your copied game data and your saves from this device.";

    /**
     * What the screen says for a failure, in one or two plain sentences with the next step. The work's own sentence
     * may name a file of the app's storage or a folder of the device (it is written for the log, where it helps);
     * a player can do nothing with those, so the screen names the kind of failure and what to do.
     */
    static String plainFailure(String error) {
        String e = error == null ? "" : error.trim();
        if (e.startsWith("Not enough free space") || e.equals(ShipStart.GAME_RUNNING) || e.equals(ShipFirstRun.BUSY)) {
            return e;   // these name amounts and the next step only
        }
        if (e.startsWith("This app only works for the device's main user")) {
            return FAILED_OTHER_USER;
        }
        if (e.startsWith("This app's file is damaged") || e.startsWith("This app's own file cannot be read")
                || e.startsWith("The game code record inside this app") || e.startsWith("The start request inside this app")
                || e.startsWith("The game code inside this app is not usable")) {
            return FAILED_APP_FILE;
        }
        if (e.contains(ShipFiles.NOT_A_FOLDER)) {
            return FAILED_IN_THE_WAY;
        }
        return FAILED_WRITING;
    }

    /** Every piece the app carries is in place (it was already, or ShipFirstRun finished): the menu follows. */
    void finished() {
        phase = Phase.FINISHED;
    }

    // ---- what the screen draws -----------------------------------------------------------------------------

    String heading() {
        switch (phase) {
            case CHECKING:
            case PREPARING: return HEADING;
            case PREPARE_FAILED: return HEADING_FAILED;
            default: return "";
        }
    }

    /** True when text() reports a failure (drawn in the error colour). */
    boolean textIsProblem() {
        return phase == Phase.PREPARE_FAILED;
    }

    String text() {
        switch (phase) {
            case CHECKING: return CHECKING;
            case PREPARING: return preparingText();
            case PREPARE_FAILED: return message + "\n\n" + (fromMenu ? WAY_BACK : WAY_OUT);
            default: return "";
        }
    }

    private String preparingText() {
        switch (step) {
            case GAME_CODE:
                return "Installing the game's own code: " + megabytesOf(stepDone, stepTotal) + "." + ONCE;
            case SHADERS:
                return "Installing the game's graphics files: " + megabytesOf(stepDone, stepTotal) + "." + ONCE;
            case GAME_DATA:
                return "Installing the small game files this app carries: " + megabytesOf(stepDone, stepTotal) + "." + ONCE;
            case START_REQUEST:
            case SWITCHES:
            case FINISH:
                return "Finishing." + ONCE;
            default:
                return CHECKING + ONCE;
        }
    }

    private static String megabytesOf(long done, long total) {
        return ShipFiles.megabytes(done) + " of " + ShipFiles.megabytes(total);
    }

    /** 0..1 while something large is copied, else -1 (no bar). */
    float fraction() {
        if (phase == Phase.PREPARING && stepTotal > 0) {
            return (float) Math.max(0.0, Math.min(1.0, (double) stepDone / (double) stepTotal));
        }
        return -1f;
    }

    /** The button's words, or null when there is none. Gamepad A presses it. */
    String primary() {
        return phase == Phase.PREPARE_FAILED ? TRY_AGAIN : null;
    }

    int primaryAction() {
        return phase == Phase.PREPARE_FAILED ? ACT_RETRY : ACT_NONE;
    }

    /** What B and Android's Back do: nothing while the work runs (it is short and must finish), close after a failure. */
    int backAction() {
        return phase == Phase.PREPARE_FAILED ? ACT_CLOSE : ACT_NONE;
    }

    /** True while work runs that must not be cut short by the screen going dark. */
    boolean busy() {
        return phase == Phase.PREPARING;
    }
}
