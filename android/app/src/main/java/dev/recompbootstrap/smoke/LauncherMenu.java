package dev.recompbootstrap.smoke;

import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;
import java.util.TimeZone;

/**
 * What the launcher screen shows and how it reacts (design section 2.4). Pure Java; LauncherScreen only draws it.
 *
 * Four items in a fixed order. The first one depends on what is installed: "Play" when everything a start needs is
 * in place. Otherwise, in a developer build, "Check again", which looks at the files once more (that build is
 * installed from a computer with the install kit, so there is nothing the app could set up itself; the page says
 * what the kit does and what the user does next). In the build that is handed out (selfInstalling: it carries its
 * game code) the first item is "Set up game files" while only the files from the player's discs are missing, and
 * opens the two-disc setup page; when one of the app's own pieces is not in place it is "Finish setting up" and
 * opens the app's first screen, which checks and repairs them. A disabled item keeps its place, can take the focus
 * so its reason can be read, and does nothing when confirmed. Every sentence shown is LaunchAdvice's in a developer
 * build and ShipAdvice's in the build that is handed out.
 */
final class LauncherMenu {
    static final int PRIMARY = 0, SETTINGS = 1, CONTROLLER_CHECK = 2, ABOUT = 3, COUNT = 4;

    /** What the screen must do after a confirm or a tap. */
    enum Action { NONE, PLAY, RECHECK, OPEN_SETTINGS, OPEN_CONTROLLER_CHECK, OPEN_ABOUT,
        /** The build that is handed out: open the two-disc setup page (game data from the player's discs is missing). */
        OPEN_GAME_FILES_SETUP,
        /** The build that is handed out: open the app's first screen, which checks and repairs the app's own pieces. */
        OPEN_FIRST_SCREEN }

    /** How the second line of an item is to be coloured. */
    enum Tone { NONE, HINT, ERROR }

    static final String NOT_IN_THIS_BUILD = LaunchAdvice.NO_GAME_WINDOW;
    static final String ALREADY_RUNNING = LaunchAdvice.ALREADY_RUNNING;
    static final String CHECKING_CODE = LaunchAdvice.CHECKING_CODE;
    static final String CHECKING_AGAIN = LaunchAdvice.CHECKING_AGAIN;
    static final String CLOSE_GAME_FIRST = LaunchAdvice.CLOSE_GAME_FIRST;
    static final String NOT_IN_FRONT = LaunchAdvice.NOT_IN_FRONT;
    static final String NOT_IN_FRONT_DETAIL = LaunchAdvice.NOT_IN_FRONT_DETAIL;
    static final String UNQUALIFIED = LaunchAdvice.OTHER_VERSION;
    static final String UNQUALIFIED_DETAIL = LaunchAdvice.OTHER_VERSION_DETAIL;

    private final boolean launchHelperPresent;
    /** True in a build that carries its game code and installs by itself: no install kit, no computer. */
    private final boolean selfInstalling;
    private final boolean publicApp;
    private InstallState install;        // null until the first detection has finished
    /** The handed-out build's own answer that came with install (its counts are what the page says); else null. */
    private ShipReady ship;
    private int focused = PRIMARY;
    private boolean preparing;           // a Play is being prepared in the background
    private String playMessage;
    private boolean playMessageIsError;
    /** The whole text for the right panel when the line under Play is only its short form; else null. */
    private String playDetail;
    /** The question "start code that is not the qualified build?" stands; the next confirm on Play answers yes. */
    private boolean asking;
    /** The Play being prepared was the answer "yes" to that question. */
    private boolean accepted;
    /** "Check again" was chosen and its look at the files has not come back yet. */
    private boolean rechecking;
    private String controllerMessage;
    /**
     * The player was sent somewhere that may have changed the files a start needs (the game, the two-disc setup page,
     * the app's first screen) and the menu has not looked at them since. See takeWholeLook.
     */
    private boolean wentWhereFilesChange;

    /** launchHelperPresent: whether the class that can start the game activity exists in this build. */
    LauncherMenu(boolean launchHelperPresent) {
        this(launchHelperPresent, false);
    }

    /**
     * selfInstalling: whether this build carries its game code (ShipApplication.carriesGameCode). Then the first item
     * opens the app's own setup while something is missing, and the text beside it says so instead of naming the
     * install kit.
     */
    LauncherMenu(boolean launchHelperPresent, boolean selfInstalling) {
        this(launchHelperPresent, selfInstalling, false);
    }

    LauncherMenu(boolean launchHelperPresent, boolean selfInstalling, boolean publicApp) {
        this.publicApp = publicApp;
        this.launchHelperPresent = launchHelperPresent;
        this.selfInstalling = selfInstalling;
    }

    /**
     * The result of a detection. null means "still looking". When it answers a "Check again" and nothing differs
     * from what was shown before, a line says so: otherwise the press would seem to have done nothing.
     */
    void setInstall(InstallState install) {
        setInstall(install, null);
    }

    /**
     * The same with the answer of the build's own rule (ShipReady) that the detection was made with: a build that
     * carries its game code says what is missing in that rule's numbers. ship is null in a developer build.
     */
    void setInstall(InstallState install, ShipReady ship) {
        this.ship = ship;
        InstallState before = this.install;
        boolean answersRecheck = rechecking;
        this.install = install;
        rechecking = false;
        if (install != null && !install.ready() && !preparing) {
            // "Nothing has changed" means that the page reads as it did: the same state line, the same advice.
            boolean unchanged = answersRecheck && before != null
                    && LaunchAdvice.stateLine(before).equals(LaunchAdvice.stateLine(install))
                    && LaunchAdvice.help(before, launchHelperPresent).equals(LaunchAdvice.help(install, launchHelperPresent));
            // a message about Play does not belong under "Check again"
            setPlayMessage(unchanged ? LaunchAdvice.CHECKED_NO_CHANGE : null, false, null);
            asking = false;
        } else if (answersRecheck && CHECKING_AGAIN.equals(playMessage)) {
            setPlayMessage(null, false, null);   // ready now: "Ready." says it
        }
    }

    /**
     * The screen became visible again. "The game is running" may no longer be true, so those two messages go.
     * A start failure stays until the next attempt: the user may have looked at another screen for the cause.
     */
    void shown() {
        leavePlayItem();
        controllerMessage = null;
    }

    /**
     * The user turned away from Play (moved the highlight, pressed B, chose another item, or the screen was shown
     * again): an open question is answered "no" and "The game is already running." goes. A start failure and
     * "Not started" stay until the next attempt.
     */
    void leavePlayItem() {
        if (asking || ALREADY_RUNNING.equals(playMessage)) {
            setPlayMessage(null, false, null);
        }
        asking = false;
    }

    /**
     * For the screen, each time it looks at the installed files: true once after the player was sent to the game, to
     * the two-disc setup page or to the app's first screen ("Finish Setting Up"). The look that follows such a
     * return reads the game code file and the shader file whole again (ShipVerify.again); every other look (a tab,
     * About, a return from Home or from the controller check, "Check again") answers from the last whole read.
     */
    boolean takeWholeLook() {
        boolean took = wentWhereFilesChange;
        wentWhereFilesChange = false;
        return took;
    }

    /** True while the launcher waits for the answer to "start code that is not the qualified build?". */
    boolean asking() {
        return asking;
    }

    /** For the screen, right after confirm returned PLAY: whether this Play may start unqualified game code. */
    boolean acceptsUnqualified() {
        return accepted;
    }

    private void setPlayMessage(String message, boolean isError, String detail) {
        playMessage = message;
        playMessageIsError = isError;
        playDetail = detail;
    }

    int focused() {
        return focused;
    }

    boolean preparing() {
        return preparing;
    }

    String stateLine() {
        if (publicApp && install != null && !install.ready()) return "Set up the game to play.";
        return selfInstalling ? ShipAdvice.stateLine(install, ship) : LaunchAdvice.stateLine(install);
    }

    /** True in the build that carries its game code: its pages use the player's words (ShipAdvice). */
    boolean selfInstalling() {
        return selfInstalling;
    }

    /** Public builds keep a direct Setup Game shortcut even after the installed files become ready. */
    boolean publicApp() {
        return publicApp;
    }

    /** True when the state line is drawn in the warning colour: something a start needs is not in place. */
    boolean stateLineIsWarning() {
        return install != null && !install.ready();
    }

    String label(int item) {
        switch (item) {
            case PRIMARY:
                if (install == null) {
                    return "Checking…";
                }
                if (install.ready()) {
                    return "Play";
                }
                if (publicApp) return "Set Up Game";
                if (!selfInstalling) {
                    return LaunchAdvice.CHECK_AGAIN;
                }
                return ShipAdvice.gameFilesMissing(install, ship) ? ShipAdvice.ITEM_GAME_FILES : ShipAdvice.ITEM_FINISH;
            case SETTINGS:
                return "Settings";
            case CONTROLLER_CHECK:
                return "Controller check";
            case ABOUT:
                return "About";
            default:
                throw new IllegalArgumentException("no such item: " + item);
        }
    }

    boolean enabled(int item) {
        switch (item) {
            case PRIMARY:
                return install != null && (!install.ready() || launchHelperPresent);
            case CONTROLLER_CHECK:
                return launchHelperPresent;
            case SETTINGS:
            case ABOUT:
                return true;
            default:
                throw new IllegalArgumentException("no such item: " + item);
        }
    }

    /** The line under an item's label: the reason it is disabled, or a message about the last attempt. null = none. */
    String secondLine(int item) {
        switch (item) {
            case PRIMARY:
                if (install != null && install.ready() && !launchHelperPresent) {
                    return NOT_IN_THIS_BUILD;
                }
                return playMessage;
            case CONTROLLER_CHECK:
                return launchHelperPresent ? controllerMessage : NOT_IN_THIS_BUILD;
            case SETTINGS:
            case ABOUT:
                return null;
            default:
                throw new IllegalArgumentException("no such item: " + item);
        }
    }

    Tone secondLineTone(int item) {
        if (secondLine(item) == null) {
            return Tone.NONE;
        }
        return item == PRIMARY && playMessageIsError && enabled(PRIMARY) ? Tone.ERROR : Tone.HINT;
    }

    /**
     * Moves the focus by one item (direction -1 = up, +1 = down). A fresh press wraps from the last item to the
     * first and back; a held direction (repeat) stops at the end. Returns true when the focus moved.
     */
    boolean move(int direction, boolean repeat) {
        if (direction != -1 && direction != 1) {
            throw new IllegalArgumentException("direction must be -1 or +1");
        }
        int next = focused + direction;
        if (next < 0 || next >= COUNT) {
            if (repeat) {
                return false;
            }
            next = next < 0 ? COUNT - 1 : 0;
        }
        leavePlayItem();
        focused = next;
        return true;
    }

    /** A tap on an item: it takes the focus and is activated. */
    Action tap(int item, boolean gameRunning) {
        if (item < 0 || item >= COUNT) {
            throw new IllegalArgumentException("no such item: " + item);
        }
        focused = item;
        return confirm(gameRunning);
    }

    /**
     * The focused item was confirmed. gameRunning: whether the game process exists right now.
     * Returns what the screen must do; NONE for a disabled item, while a Play is being prepared, and when the game
     * is already running (a message under the item then says so).
     */
    Action confirm(boolean gameRunning) {
        if (preparing || !enabled(focused)) {
            return Action.NONE;
        }
        boolean answeredYes = asking;   // only read below when the confirmed item is Play
        leavePlayItem();
        switch (focused) {
            case PRIMARY:
                if (!install.ready() && selfInstalling) {
                    // Nothing is looked at again here: the page that opens does the work, and the return from it
                    // runs the detection (the screen's onShown / onAppBack).
                    setPlayMessage(null, false, null);
                    wentWhereFilesChange = true;   // both pages write files of the app's storage
                    return publicApp || ShipAdvice.gameFilesMissing(install, ship) ? Action.OPEN_GAME_FILES_SETUP : Action.OPEN_FIRST_SCREEN;
                }
                if (!install.ready()) {
                    rechecking = true;
                    setPlayMessage(CHECKING_AGAIN, false, null);
                    return Action.RECHECK;
                }
                if (gameRunning) {
                    setPlayMessage(ALREADY_RUNNING, false, null);
                    return Action.NONE;
                }
                preparing = true;
                accepted = answeredYes;
                setPlayMessage(CHECKING_CODE, false, null);
                return Action.PLAY;
            case SETTINGS:
                return Action.OPEN_SETTINGS;
            case CONTROLLER_CHECK:
                if (gameRunning) {
                    // The controller check runs in the game's process; starting it would disturb a running game.
                    controllerMessage = CLOSE_GAME_FIRST;
                    return Action.NONE;
                }
                controllerMessage = null;
                return Action.OPEN_CONTROLLER_CHECK;
            default:
                return Action.OPEN_ABOUT;
        }
    }

    /**
     * The background preparation of a Play has finished. error is null on success. The preparation takes a moment,
     * so two things are asked again: gameRunning, and inFront (whether the launcher still has the window focus; a
     * game must not pop up after the user has left the launcher). Returns true when the screen must now start the game.
     */
    boolean prepared(String error, boolean gameRunning, boolean inFront) {
        if (!preparing) {
            return false;
        }
        preparing = false;
        accepted = false;
        if (!publicApp && LaunchRequest.UNQUALIFIED.equals(error)) {
            // Nothing was created. The question is put only to a user who is looking at the launcher.
            asking = inFront;
            if (inFront) {
                setPlayMessage(UNQUALIFIED, false, UNQUALIFIED_DETAIL);
            } else {
                setPlayMessage(NOT_IN_FRONT, false, NOT_IN_FRONT_DETAIL);
            }
            return false;
        }
        if (error != null) {
            startFailed(error);
            return false;
        }
        if (gameRunning) {
            setPlayMessage(ALREADY_RUNNING, false, null);
            return false;
        }
        if (!inFront) {
            setPlayMessage(NOT_IN_FRONT, false, NOT_IN_FRONT_DETAIL);
            return false;
        }
        setPlayMessage(null, false, null);
        wentWhereFilesChange = true;   // the game is started now: the look after the return reads the files whole
        return true;
    }

    /**
     * The game could not be started; reason is what LaunchRequest.prepare or the screen gave. Shown as the
     * sentence LaunchAdvice has for that reason: what happened and what to do next.
     */
    void startFailed(String reason) {
        preparing = false;
        accepted = false;
        asking = false;
        setPlayMessage(publicApp ? PublicCodeWords.launch(reason)
                : selfInstalling ? ShipAdvice.startFailure(reason) : LaunchAdvice.startFailure(reason), true, null);
    }

    /**
     * The launcher is shown again after a start it made, and the look at what the game window left behind is done.
     * When that start is known to have failed (the game window did not accept or refused it, or Android reports
     * that the game's process crashed or was closed), the sentence for it stands on the Play page until the next
     * attempt. Nothing is said when nothing is known to be wrong, and nothing while another Play is under way or
     * something a start needs is missing (the state line then says what).
     */
    void startOutcome(LaunchOutcome outcome) {
        String text = outcome == null ? null : selfInstalling ? ShipAdvice.afterStart(outcome) : LaunchAdvice.afterStart(outcome);
        if (text == null || preparing || asking || install == null || !install.ready()) {
            return;
        }
        setPlayMessage(text, true, null);
    }

    /** The controller check screen could not be opened; reason is one sentence, shown under that item with the next step. */
    void controllerCheckFailed(String reason) {
        controllerMessage = selfInstalling ? ShipAdvice.controllerCheckFailed(reason) : LaunchAdvice.controllerCheckFailed(reason);
    }

    /**
     * The whole text for the right panel (the line under Play is one line and is cut when long), or null: the last
     * start failure, the question about unqualified game code, or why a prepared start was not made.
     */
    String failure() {
        return playDetail != null ? playDetail : playMessageIsError ? playMessage : null;
    }

    /** True when failure() is a failure (error colour); false for the question and the "not started" note. */
    boolean failureIsError() {
        return playDetail == null && playMessageIsError;
    }

    /** Right panel, line 1: found, incomplete (some of the files a start names are missing) or missing. */
    String dataFact() {
        if (selfInstalling) {
            return ShipAdvice.dataFact(install, ship);
        }
        if (install == null) {
            return "Game data: checking…";
        }
        return install.dataPresent ? "Game data: found"
                : install.missingFiles > 0 ? "Game data: incomplete" : "Game data: missing";
    }

    /** Right panel, line 2, for example "Game code: 338 MB, 2026-10-04 17:26". zone is the device's time zone. */
    String codeFact(TimeZone zone) {
        if (selfInstalling) {
            return ShipAdvice.codeFact(install, ship);
        }
        if (install == null) {
            return "Game code: checking…";
        }
        if (install.codeBytes <= 0) {
            return "Game code: missing";
        }
        if (install.codeFault != InstallState.CodeFault.NONE) {
            return "Game code: cannot be used";
        }
        return "Game code: " + codeSizeAndDate(install.codeBytes, install.codeModifiedMs, zone);
    }

    /** Right panel, line 3. name is the connected gamepad's name, or null. */
    static String controllerFact(String name) {
        return "Controller: " + (name == null || name.trim().isEmpty() ? "none" : name.trim());
    }

    /** Right panel, the text under the facts: what the state means and what to do next. */
    String detail() {
        if (selfInstalling) {
            if (publicApp && (install == null || !install.ready())) return PublicCodeWords.HELP;
            return ShipAdvice.help(install, ship);
        }
        return LaunchAdvice.help(install, launchHelperPresent);
    }

    /** "338 MB, 2026-10-04 17:26": megabytes of 1,000,000 bytes rounded to the nearest, and the local date. */
    static String codeSizeAndDate(long bytes, long modifiedMs, TimeZone zone) {
        SimpleDateFormat format = new SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT);
        format.setTimeZone(zone);
        return ((bytes + 500000L) / 1000000L) + " MB, " + format.format(new Date(modifiedMs));
    }
}
