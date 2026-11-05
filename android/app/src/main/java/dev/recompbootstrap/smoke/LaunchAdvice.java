package dev.recompbootstrap.smoke;

import java.util.Locale;

/**
 * Every sentence the launcher says about what is installed, why the game was not started and what to do next.
 * Pure Java; LauncherMenu and AboutScreen show these texts and decide nothing about their wording.
 *
 * This week's build is installed from a computer: the install kit puts the game data, the game code, the shader
 * file, the start settings and the version record on the device over USB. The app cannot do any of that itself, so
 * for each missing piece the text says that the kit copies it and what the user does. The three KIT_ texts are the
 * only places that say how the kit is used; if the kit is used differently, they are what must change.
 *
 * Each text has two parts: what is the case (one sentence a player understands) and the next step. A reason that
 * only helps support (an error's class, a status word of the game window) stands in brackets at the very end.
 */
final class LaunchAdvice {
    private LaunchAdvice() {}

    // ------------------------------------------------------------------------------------------ the install kit

    static final String KIT_RUN = "Connect this device to the computer with a USB cable and run the install kit there.";
    static final String KIT_AGAIN = "Connect this device to the computer with a USB cable and run the install kit again.";
    static final String KIT_APP = "Install the app again with the install kit.";
    static final String THEN_CHECK = "When it has finished, press A here to check again.";
    private static final String IF_AGAIN = " If it happens again, c" + KIT_AGAIN.substring(1);

    // ------------------------------------------------------------------------------------------ the install state

    static final String STATE_CHECKING = "Checking game files…";
    static final String STATE_READY = "Ready.";
    static final String STATE_NOTHING = "The game is not installed on this device.";
    static final String STATE_DISC_COPY = "This version cannot start the game from the disc files copied here.";
    static final String STATE_DATA_MISSING = "The game code is here. The game data is missing.";
    static final String STATE_DATA_EMPTY = "The start settings on this device name no game data.";
    static final String STATE_CODE_MISSING = "The game data is here. The game code is missing.";
    static final String STATE_CODE_UNLOADABLE = "The game code file on this device cannot be used.";
    static final String STATE_SHADERS_MISSING = "The shader file is missing.";

    /** The line at the top of the Play page. install is null while the first look at the files runs. */
    static String stateLine(InstallState install) {
        if (install == null) {
            return STATE_CHECKING;
        }
        switch (install.problem) {
            case NONE:
                return STATE_READY;
            case NOTHING_INSTALLED:
                return STATE_NOTHING;
            case DISC_COPY_ONLY:
                return STATE_DISC_COPY;
            case DATA_MISSING:
                return STATE_DATA_MISSING;
            case DATA_INCOMPLETE:
                return install.missingFiles > 0 ? missingLine(install.missingFiles, install.referencedFiles)
                        : STATE_DATA_EMPTY;
            case CODE_MISSING:
                return STATE_CODE_MISSING;
            case CODE_UNLOADABLE:
                return STATE_CODE_UNLOADABLE;
            default:
                return STATE_SHADERS_MISSING;
        }
    }

    /** "1 of 3 game data files is missing." / "2 of 3 game data files are missing." */
    static String missingLine(int missing, int referenced) {
        return missing + " of " + referenced + " game data files " + (missing == 1 ? "is" : "are") + " missing.";
    }

    static final String HELP_READY = "Leaving the game with Android's Back or Home closes it at once: progress since"
            + " the last save is lost.";
    /** Under the line NO_GAME_WINDOW, which carries the next step: why Play is not offered although the game is installed. */
    static final String HELP_NO_GAME_WINDOW = "This copy of the app cannot open the game window, so Play and the"
            + " controller check are not offered.";
    static final String HELP_PIN_IGNORED = "The start settings chosen at the install could not be used, so the newest"
            + " ones on this device are used. To restore them, run the install kit again.";
    static final String HELP_NO_RECORD = "No version of the game code is on record for this install, so Play does not"
            + " compare it.";
    static final String HELP_NOTHING = "This version of the app does not install the game itself. " + KIT_RUN
            + " It copies the game data, the game code and the shader file to this device. " + THEN_CHECK;
    static final String HELP_DISC_COPY = "The disc files stay where they are. To play with this version, the game is"
            + " installed from a computer. " + KIT_RUN + " " + THEN_CHECK;
    static final String HELP_DATA = "The game data is the files from the game discs and the start settings. The"
            + " install kit copies them to this device. " + KIT_AGAIN + " " + THEN_CHECK;
    static final String HELP_CODE_MISSING = "The game code is the game's program for this device. The install kit"
            + " copies it here. " + KIT_AGAIN + " " + THEN_CHECK;
    static final String HELP_CODE_REPLACED = "The install kit puts the right file in place. " + KIT_AGAIN + " "
            + THEN_CHECK;
    static final String HELP_SHADERS = "The game loads its shaders from this file, so Play is not offered without it."
            + " The install kit copies it to this device. " + KIT_AGAIN + " " + THEN_CHECK;

    /** One sentence for each reason the game window's loader refuses a game code file; "" for NONE. */
    static String faultSentence(InstallState.CodeFault fault) {
        switch (fault) {
            case NOT_A_PROGRAM:
                return "It is not a game program for this device.";
            case PERMISSIONS:
                return "Its file permissions are not the ones the game window accepts.";
            case LINKED:
                return "It has more than one name on this device, which the game window does not accept.";
            case TOO_LARGE:
                return "It is larger than the game window accepts (512 MiB).";
            case FOLDER_PERMISSIONS:
                return "The permissions of its folder are not the ones the game window accepts.";
            default:
                return "";
        }
    }

    /**
     * The text under the facts of the Play page: what the state means and what to do next. gameWindowInApp: whether
     * this copy of the app has the part that opens the game window. "" while the first look at the files runs.
     */
    static String help(InstallState install, boolean gameWindowInApp) {
        if (install == null) {
            return "";
        }
        switch (install.problem) {
            case NONE:
                if (!gameWindowInApp) {
                    return HELP_NO_GAME_WINDOW;
                }
                // One note at most, the one that matters more, so that the text always fits its four lines.
                return HELP_READY + (install.pinIgnored ? " " + HELP_PIN_IGNORED
                        : !install.versionOnRecord ? " " + HELP_NO_RECORD : "");
            case NOTHING_INSTALLED:
                return HELP_NOTHING;
            case DISC_COPY_ONLY:
                return HELP_DISC_COPY;
            case DATA_MISSING:
            case DATA_INCOMPLETE:
                return HELP_DATA;
            case CODE_MISSING:
                return HELP_CODE_MISSING;
            case CODE_UNLOADABLE:
                return faultSentence(install.codeFault) + " " + HELP_CODE_REPLACED;
            default:
                return HELP_SHADERS;
        }
    }

    // ------------------------------------------------------------------------------------------ Play

    static final String CHECK_AGAIN = "Check again";
    static final String CHECKING_AGAIN = "Checking\u2026";
    static final String CHECKED_NO_CHANGE = "Checked again: nothing has changed.";
    static final String CHECKING_CODE = "Checking game code…";
    static final String ALREADY_RUNNING = "The game is already running. Close it before starting it again.";
    static final String NOT_IN_FRONT = "Not started. Choose Play again.";
    static final String NOT_IN_FRONT_DETAIL =
            "Not started: this app was left while the game code was checked. Choose Play again.";
    static final String NO_GAME_WINDOW = "Not available in this copy of the app. " + KIT_APP;
    static final String CLOSE_GAME_FIRST = "Close the game first.";
    static final String OTHER_VERSION = "Not the version on record. A: start anyway";
    static final String OTHER_VERSION_DETAIL = "The game code on this device is not the version on record for this"
            + " install. Starting it can overwrite the autosave. Press A on Play again to start it anyway, or B to"
            + " leave it. To put the recorded version back, run the install kit again.";

    private static final String NOT_STARTED = "Not started: ";
    static final String FAIL_CODE_MISSING = NOT_STARTED + "the game code is missing. " + KIT_AGAIN;
    static final String FAIL_DATA = NOT_STARTED + "game data is missing. " + KIT_AGAIN;
    static final String FAIL_SHADERS = NOT_STARTED + "the shader file is missing. " + KIT_AGAIN;
    static final String FAIL_CODE_UNUSABLE = NOT_STARTED + "the game code file on this device cannot be used.";
    static final String FAIL_START_SETTINGS = NOT_STARTED + "the start settings on this device cannot be used. The"
            + " install kit writes new ones. " + KIT_AGAIN;
    static final String FAIL_SETTINGS_FOLDER = NOT_STARTED + "the app could not create its settings folder. Check"
            + " that this device has free storage, then choose Play again.";
    static final String FAIL_FILE = NOT_STARTED + "a game file could not be read or copied. Check that this device"
            + " has free storage, then choose Play again." + IF_AGAIN;
    private static final String FAIL_OTHER_NEXT = ". Choose Play again." + IF_AGAIN;

    /**
     * The whole text for a reason LaunchRequest.prepare (or the start itself) gave: what happened, in a sentence a
     * player understands, and the next step. Never null. LaunchRequest.UNQUALIFIED is a question, not a failure,
     * and is not handled here.
     */
    static String startFailure(String reason) {
        if (reason == null || reason.trim().isEmpty()) {
            return NOT_STARTED + "no reason was given" + FAIL_OTHER_NEXT;
        }
        if (reason.equals(LaunchRequest.ERROR_CODE_MISSING)) {
            return FAIL_CODE_MISSING;
        }
        if (reason.equals(LaunchRequest.ERROR_NOT_INSTALLED)) {
            return FAIL_DATA;
        }
        if (reason.equals(LaunchRequest.ERROR_SHADERS_MISSING)) {
            return FAIL_SHADERS;
        }
        if (reason.equals(LaunchRequest.ERROR_CODE_TOO_LARGE)) {
            return codeUnusable(InstallState.CodeFault.TOO_LARGE);
        }
        if (reason.startsWith(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX)) {
            String name = reason.substring(LaunchRequest.ERROR_CODE_UNLOADABLE_PREFIX.length());
            InstallState.CodeFault fault = InstallState.CodeFault.NONE;
            for (InstallState.CodeFault known : InstallState.CodeFault.values()) {
                if (known.name().toLowerCase(Locale.ROOT).equals(name)) {
                    fault = known;
                }
            }
            return codeUnusable(fault);
        }
        if (reason.startsWith(LaunchRequest.ERROR_REQUEST_REFUSED_PREFIX) || reason.equals(LaunchRequest.REWRITE_EMPTY)
                || reason.equals(LaunchRequest.REWRITE_BAD_ID) || reason.equals(LaunchRequest.REWRITE_SAME_ID)
                || reason.equals(LaunchRequest.REWRITE_ID_ABSENT) || reason.equals(LaunchRequest.REWRITE_ID_LEFT)) {
            return FAIL_START_SETTINGS;
        }
        if (reason.equals(LaunchRequest.ERROR_SETTINGS_DIRECTORY)) {
            return FAIL_SETTINGS_FOLDER;
        }
        if (reason.startsWith(LaunchRequest.ERROR_IO_PREFIX)) {
            String detail = reason.substring(LaunchRequest.ERROR_IO_PREFIX.length());
            if (detail.endsWith(")")) {
                detail = detail.substring(0, detail.length() - 1);
            }
            return FAIL_FILE + " (" + withoutDirectories(detail) + ")";
        }
        return NOT_STARTED + reason + FAIL_OTHER_NEXT;
    }

    /**
     * An error's text with a file's path cut down to the file's own name: "NoSuchFileException: /data/.../run-x/a.bin"
     * becomes "NoSuchFileException: a.bin". The directories are the app's own and say nothing to the player; what
     * follows the path (": No space left on device") is kept.
     */
    static String withoutDirectories(String detail) {
        int slash = detail.indexOf('/');
        if (slash < 0) {
            return detail;
        }
        int end = slash;
        while (end < detail.length() && detail.charAt(end) != ' ' && detail.charAt(end) != ':') {
            ++end;
        }
        String path = detail.substring(slash, end);
        return detail.substring(0, slash) + path.substring(path.lastIndexOf('/') + 1) + detail.substring(end);
    }

    private static String codeUnusable(InstallState.CodeFault fault) {
        String why = faultSentence(fault);
        return FAIL_CODE_UNUSABLE + (why.isEmpty() ? "" : " " + why) + " " + HELP_CODE_REPLACED;
    }

    /** Android would not open a screen; error is the exception's class name, kept for support. */
    static String cannotOpen(String screen, String error) {
        return "Android would not open the " + screen + " (" + error + ")";
    }

    /** The line on the Controller page when Android would not open the controller check. */
    static String controllerCheckFailed(String reason) {
        return reason + ". " + KIT_APP;
    }

    // ------------------------------------------------------------------------------------------ after a start

    private static final String DID_NOT_START = "The game did not start: ";
    static final String OUTCOME_NOT_ACCEPTED = DID_NOT_START + "the game window did not accept the start. Choose Play"
            + " again. If it happens again, i" + KIT_APP.substring(1);
    static final String OUTCOME_CODE_REFUSED = DID_NOT_START + "the game window refused the game code file. "
            + KIT_AGAIN;
    static final String OUTCOME_SETTINGS_REFUSED = DID_NOT_START + "the game window refused the start settings."
            + " Choose Play again." + IF_AGAIN;
    static final String OUTCOME_CODE_STOPPED = DID_NOT_START + "the game code stopped while it was starting. Choose"
            + " Play again." + IF_AGAIN;
    static final String OUTCOME_WINDOW = DID_NOT_START + "the game window could not be set up. Choose Play again."
            + IF_AGAIN;
    static final String OUTCOME_STOPPED = "The game stopped by itself. Choose Play to start it again.";
    static final String OUTCOME_CRASH = "The game closed because of an error in the game code (a crash)";
    static final String OUTCOME_LOW_MEMORY = "Android closed the game to free memory";
    static final String OUTCOME_NOT_RESPONDING = "Android closed the game because it had stopped responding";

    /**
     * What to say when the launcher is shown again after a start, or null when nothing is known to have gone wrong
     * (the usual case: the player left the game).
     */
    static String afterStart(LaunchOutcome outcome) {
        switch (outcome.kind) {
            case NOT_ACCEPTED:
                return OUTCOME_NOT_ACCEPTED;
            case REFUSED:
                return refusal(outcome.word) + " (" + outcome.word + ")";
            case STOPPED:
                return OUTCOME_STOPPED;
            case CRASHED:
                return OUTCOME_CRASH + after(outcome.ranMs) + ". Choose Play to start it again.";
            case LOW_MEMORY:
                return OUTCOME_LOW_MEMORY + after(outcome.ranMs) + ". Close other apps, then choose Play again.";
            case NOT_RESPONDING:
                return OUTCOME_NOT_RESPONDING + after(outcome.ranMs) + ". Choose Play to start it again.";
            default:
                return null;
        }
    }

    /** The sentence for a status word of the game window (PrivateTraceActivity.show: "REJECT_..."). */
    private static String refusal(String word) {
        if (word.startsWith("REJECT_LIBRARY") || word.equals("REJECT_SIZE") || word.equals("REJECT_HASH")) {
            return OUTCOME_CODE_REFUSED;
        }
        if (word.startsWith("REJECT_MANIFEST")) {
            return OUTCOME_SETTINGS_REFUSED;
        }
        if (word.equals("REJECT_NATIVE_RUN") || word.startsWith("REJECT_RUN_SETUP")
                || word.equals("REJECT_EMPTY_PROTOCOL")) {
            return OUTCOME_CODE_STOPPED;
        }
        return OUTCOME_WINDOW;
    }

    /** " after 40 seconds", " after 12 minutes"; "" when the time is not known. */
    static String after(long ranMs) {
        if (ranMs < 0) {
            return "";
        }
        long seconds = ranMs / 1000;
        if (seconds < 90) {
            return " after " + seconds + (seconds == 1 ? " second" : " seconds");
        }
        return " after " + (seconds + 30) / 60 + " minutes";
    }

    // ------------------------------------------------------------------------------------------ About

    static final String ABOUT_CHECKING = "Checking the installed game…";
    static final String ABOUT_VERSION_COMPARED = "Version check: Play compares the game code with the version on"
            + " record for this install.";
    static final String ABOUT_VERSION_NOT_COMPARED = "Version check: none. No version of the game code is on record"
            + " for this install.";
    static final String ABOUT_NAMES_NONE = "This game version has none of the game settings of the Settings tab: each"
            + " is shown as not available there.";
    static final String ABOUT_NAMES = "This game version does not have these settings. They are shown as not"
            + " available in Settings:";
    static final String ABOUT_NAMES_ALL = "This game version has every game setting of the Settings tab.";

    /** The facts of the About tab's "Game Build" section, one per line, under the game code line. */
    static String aboutInstall(InstallState install) {
        StringBuilder text = new StringBuilder();
        text.append("Game data: ");
        if (install.dataPresent) {
            text.append(install.referencedFiles == 1 ? "1 file or folder found."
                    : install.referencedFiles + " files and folders found.");
        } else if (install.referencedFiles > 0) {
            text.append(install.missingFiles).append(" of ").append(install.referencedFiles).append(" files missing.");
        } else {
            text.append("not installed.");
        }
        text.append("\nShader file: ").append(install.shaderFilePresent ? "found." : "missing.");
        if (install.codeFault != InstallState.CodeFault.NONE) {
            text.append("\nGame code file: ").append(faultSentence(install.codeFault));
        }
        text.append('\n').append(install.versionOnRecord ? ABOUT_VERSION_COMPARED : ABOUT_VERSION_NOT_COMPARED);
        return text.toString();
    }
}
