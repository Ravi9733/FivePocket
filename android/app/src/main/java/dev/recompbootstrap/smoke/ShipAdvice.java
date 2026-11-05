package dev.recompbootstrap.smoke;

/**
 * Every sentence the menu says in the build that is handed out (the one that carries its game code and installs by
 * itself): what is installed, what is missing, why the game was not started and what to do next. Pure Java.
 * LauncherMenu and AboutScreen show these texts in that build and LaunchAdvice's in a developer build; they decide
 * nothing about the wording.
 *
 * The player of this build has the app and the images of their own two game discs, nothing else: no computer, no
 * install kit. So no sentence here names a kit, a computer, a file of the app's own storage or a version of the
 * game code. Each says what is the case and what to do next; a word that only helps support stands in brackets at
 * the very end. ShipWordsTest reads every sentence this class can give against that rule.
 */
final class ShipAdvice {
    private ShipAdvice() {}

    // ------------------------------------------------------------------------------------------ the first item

    /** The Play tab's first item while only files from the player's discs are missing. Shown as "Set Up Game Files". */
    static final String ITEM_GAME_FILES = "Set up game files";
    /** The first item while the app's own pieces are not all in place. Shown as "Finish Setting Up". */
    static final String ITEM_FINISH = "Finish setting up";

    // ------------------------------------------------------------------------------------------ the state

    static final String STATE_DATA_NONE = "Your game data is not on this device yet.";
    static final String STATE_APP = "The app has not finished setting itself up.";
    static final String STATE_APP_FILE = "The app cannot read its own file.";
    /** Under the facts while no controller is connected (both builds). Play is still offered. */
    static final String NO_CONTROLLER = "Connect a controller to play: the game needs one.";

    static final String CODE_INSTALLED = "The game's own code is installed.";
    static final String HELP_APP = "This app carries the game's own code and puts it in place by itself. That has not"
            + " finished, or one of its files was changed. Choose Finish Setting Up: the app checks its files and puts"
            + " back what is missing.";
    /**
     * How to install again without losing anything. Every sentence of this build that sends the player to install
     * again says it in these words: a player who removes the app first loses the copied game files (16.4 GB) and the
     * saves, so "your saves are kept" alone would be a promise the screen cannot keep.
     */
    static final String INSTALL_OVER = "Install the app again over this one, without removing it first: your game data"
            + " and your saves are then kept.";
    /** The same as the second half of a sentence ("...; if it happens again, install the app again over this one, ..."). */
    static final String INSTALL_OVER_LOWER = "i" + INSTALL_OVER.substring(1);
    static final String HELP_APP_FILE = INSTALL_OVER;
    private static final String IF_AGAIN = " If it happens again, " + INSTALL_OVER_LOWER;
    /**
     * The next step after a refusal that leaves the Play tab's first item reading "Finish Setting Up" (the app's own
     * pieces are then known not to be in place: Play has removed the mark of the install). The page names that item,
     * which the controller can choose. No sentence of this build says "close the app and open it again" any more:
     * the menu cannot be closed with the controller, and coming back to it through the home screen does not run the
     * check (flow check, 2026-10-06).
     */
    private static final String FINISH = "Choose Finish Setting Up on this page: the app checks its own files and puts back"
            + " what is missing." + IF_AGAIN;
    /**
     * For a developer sentence that sends the player to a computer, wherever the handed-out build may still show one
     * (ShipWords): the same way, named from any tab.
     */
    static final String FINISH_FROM_ANY_TAB = "Go to the Play tab: when one of the app's own files is missing, its first item"
            + " reads Finish Setting Up, which checks the files and puts back what is missing.";

    /** True when the only thing missing is game data from the player's discs: the two-disc setup is the next step. */
    static boolean gameFilesMissing(InstallState install, ShipReady ship) {
        return install != null && ship != null && install.problem == InstallState.Problem.DATA_INCOMPLETE
                && ship.stands == ShipReady.Stands.DATA && ship.data != null;
    }

    /** "18 files", "1 file". */
    private static String files(int count) {
        return count + (count == 1 ? " file" : " files");
    }

    /** The line at the top of the Play page. install is null while the first look at the files runs. */
    static String stateLine(InstallState install, ShipReady ship) {
        if (install == null) {
            return LaunchAdvice.STATE_CHECKING;
        }
        if (install.ready()) {
            return LaunchAdvice.STATE_READY;
        }
        if (gameFilesMissing(install, ship)) {
            ShipData data = ship.data;
            if (data.discFilesMissing >= data.discFiles) {
                return STATE_DATA_NONE;
            }
            return data.discFilesMissing + " of the " + data.discFiles + " game data files "
                    + (data.discFilesMissing == 1 ? "is" : "are") + " still missing.";
        }
        return ship != null && ship.stands == ShipReady.Stands.APP_FILE ? STATE_APP_FILE : STATE_APP;
    }

    /** The text under the facts of the Play page: what the state means and what to do next. "" while checking. */
    static String help(InstallState install, ShipReady ship) {
        if (install == null) {
            return "";
        }
        if (install.ready()) {
            return LaunchAdvice.HELP_READY;
        }
        if (gameFilesMissing(install, ship)) {
            ShipData data = ship.data;
            if (data.discFilesMissing >= data.discFiles) {
                // The whole amount, rounded up as a need is ("16.4 GB"): the figure the setup page's Finish row gives.
                return CODE_INSTALLED + " Your game data is not on this device yet: " + files(data.discFiles)
                        + " from your two game discs, " + ShipFiles.amount(data.discBytesMissing, true)
                        + ". Choose Set Up Game Files to copy them from the images of your two discs.";
            }
            // What is left, written as the setup page writes a disc's amount ("8 files, 8.2 GB"), so that the two
            // pages show the same figure for the same files.
            return CODE_INSTALLED + " " + data.discFilesMissing + " of the " + data.discFiles + " files from your two"
                    + " game discs " + (data.discFilesMissing == 1 ? "is" : "are") + " still missing ("
                    + SetupModel.formatBytes(data.discBytesMissing) + "). Choose Set Up Game Files to copy the rest.";
        }
        return ship != null && ship.stands == ShipReady.Stands.APP_FILE ? HELP_APP_FILE : HELP_APP;
    }

    /** Right panel, line 1, for example "Game data: 10 of 18 files". */
    static String dataFact(InstallState install, ShipReady ship) {
        if (install == null) {
            return "Game data: checking…";
        }
        if (ship == null || ship.data == null) {
            return "Game data: not checked yet";
        }
        ShipData data = ship.data;
        return "Game data: " + (data.discFiles - data.discFilesMissing) + " of " + data.discFiles + " files";
    }

    /**
     * Right panel, line 2: whether the game's own code, which the app carries, is in place. "Not set up yet" covers
     * both a first start that did not finish and one piece that is missing or changed later: "not installed yet"
     * read as untrue while the game code file itself was in place (flow check, 2026-10-06).
     */
    static String codeFact(InstallState install, ShipReady ship) {
        if (install == null) {
            return "Game code: checking…";
        }
        return "Game code: " + (codeInstalled(ship) ? "installed" : CODE_NOT_SET_UP);
    }

    static final String CODE_NOT_SET_UP = "not set up yet";

    /** True when the app's own pieces are in place (everything the first start puts there). */
    static boolean codeInstalled(ShipReady ship) {
        return ship != null && (ship.stands == ShipReady.Stands.NOTHING || ship.stands == ShipReady.Stands.DATA);
    }

    // ------------------------------------------------------------------------------------------ Play

    private static final String NOT_STARTED = "Not started: ";

    /**
     * The whole text for a reason Play's preparation (ShipPlay, LaunchRequest) or the start itself gave. Never null.
     * A reason this build's own rules give gets its own sentence; every other one is a file of the app's storage
     * that is not as it must be, and the next step is the same: open the app again, which checks and repairs.
     */
    static String startFailure(String reason) {
        if (reason == null || reason.trim().isEmpty()) {
            return NOT_STARTED + "no reason was given. Choose Play again." + IF_AGAIN;
        }
        if (reason.startsWith(ShipPlay.DATA_MISSING_PREFIX)) {
            return NOT_STARTED + "your game data is not complete. Choose Set Up Game Files on this page: it says what"
                    + " is missing and copies it from your disc images.";
        }
        if (reason.equals(ShipPlay.NO_APP_FILE) || reason.startsWith(ShipPlay.APP_FILE_UNREADABLE_PREFIX)) {
            return NOT_STARTED + "this app cannot read its own file. " + HELP_APP_FILE;
        }
        if (reason.equals(LaunchRequest.ERROR_SETTINGS_DIRECTORY) || reason.startsWith(LaunchRequest.ERROR_IO_PREFIX)) {
            return NOT_STARTED + "a file could not be read or written. Check that this device has free storage, then"
                    + " choose Play again." + IF_AGAIN;
        }
        if (reason.startsWith("Android would not open")) {
            return NOT_STARTED + reason + ". Choose Play again." + IF_AGAIN;
        }
        // After these the app's own pieces are known not to be in place (Play has removed the mark of the install, or
        // it was never written): the menu looks again and its first item reads Finish Setting Up. They are the four
        // reasons of this build's own rules and every reason of LaunchRequest about a file that is missing or cannot
        // be used (ShipPlay.fileNotUsable; ShipPlay removes the mark for those too).
        return NOT_STARTED + "one of the app's own files is missing or was changed on this device. " + FINISH;
    }

    // ------------------------------------------------------------------------------------------ after a start

    private static final String DID_NOT_START = "The game did not start: ";
    static final String OUTCOME_NOT_ACCEPTED = DID_NOT_START + "the game window did not open as it should. Choose Play"
            + " again." + IF_AGAIN;
    /**
     * The game window refused a file the menu had just checked. Play checks the game code and the shader file by
     * their content before every start, so Play again either starts the game or finds the file and offers Finish
     * Setting Up; both can be done with the controller.
     */
    static final String OUTCOME_REFUSED_FILES = DID_NOT_START + "one of the app's own files was refused. Choose Play again: the"
            + " app checks its files before every start." + IF_AGAIN;
    static final String OUTCOME_REFUSED_START = DID_NOT_START + "the game stopped while it was starting. Choose Play"
            + " again." + IF_AGAIN;

    /**
     * What to say when the menu is shown again after a start, or null when nothing is known to have gone wrong (the
     * usual case: the player left the game).
     */
    static String afterStart(LaunchOutcome outcome) {
        switch (outcome.kind) {
            case NOT_ACCEPTED:
                return OUTCOME_NOT_ACCEPTED;
            case REFUSED: {
                String word = outcome.word;
                boolean files = word.startsWith("REJECT_LIBRARY") || word.equals("REJECT_SIZE") || word.equals("REJECT_HASH")
                        || word.startsWith("REJECT_MANIFEST");
                return (files ? OUTCOME_REFUSED_FILES : OUTCOME_REFUSED_START) + " (" + word + ")";
            }
            case STOPPED:
            case CRASHED:
            case LOW_MEMORY:
            case NOT_RESPONDING:
                return LaunchAdvice.afterStart(outcome);   // these name no kit and no file
            default:
                return null;
        }
    }

    /** The line on the Controller page when Android would not open the controller check. */
    static String controllerCheckFailed(String reason) {
        return reason + ". Try again; if it happens again, " + INSTALL_OVER_LOWER;
    }

    // ------------------------------------------------------------------------------------------ About

    static final String ABOUT_APP = "This app runs your own copy of the game from the images of your two game discs.";

    /** The facts of About, Game Build: the game's own code, the game data from the discs, the shader file. */
    static String aboutBuild(InstallState install, ShipReady ship) {
        StringBuilder text = new StringBuilder();
        text.append("Game code: ").append(codeInstalled(ship) ? "installed." : CODE_NOT_SET_UP + ".");
        text.append("\nGame data: ");
        if (ship == null || ship.data == null) {
            text.append("not checked yet.");
        } else {
            ShipData data = ship.data;
            text.append(data.discFiles - data.discFilesMissing).append(" of ").append(files(data.discFiles))
                    .append(" from your two game discs.");
        }
        text.append("\nShader file: ").append(install != null && install.shaderFilePresent ? "present." : "missing.");
        return text.toString();
    }
}
