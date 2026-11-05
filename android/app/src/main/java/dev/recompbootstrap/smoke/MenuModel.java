package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Calendar;
import java.util.Collections;
import java.util.GregorianCalendar;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * Everything the settings screen decides (design section 2.5): the rows, which one has the focus, the edited copy and
 * the saved copy of the settings, the open question and what the screen must do next. Pure Java: it imports nothing
 * from android.*, so every rule here runs in a host test. The view only draws what the getters return.
 *
 * <p>Life cycle: created while the installed game build is being checked ({@link #PHASE_LOADING}); the screen then
 * calls {@link #loaded} or {@link #loadFailed}. Commands arrive through {@link #nav} (controller) and the tap methods
 * (touch). After each of them the screen reads {@link #pendingAction()}: for {@link #ACTION_SAVE} it calls
 * {@link #beginSave()}, writes that off the UI thread and reports back with {@link #saveFinished}; for
 * {@link #ACTION_LEAVE} it closes itself. Nothing in this class touches a file.
 */
final class MenuModel {
    static final int ACTION_NONE = 0, ACTION_SAVE = 1, ACTION_LEAVE = 2;
    static final int OVERLAY_NONE = 0, OVERLAY_RESTORE = 1, OVERLAY_SAVE_QUESTION = 2, OVERLAY_DRIFT_LIST = 3;
    static final int KIND_BANNER = 0, KIND_CHOICE = 1, KIND_SLIDER = 2, KIND_TOGGLE = 3, KIND_ACTION = 4;
    static final int TONE_NORMAL = 0, TONE_WARNING = 1, TONE_ERROR = 2, TONE_DISABLED = 3, TONE_GOOD = 4;
    static final int PHASE_LOADING = 0, PHASE_READY = 1, PHASE_FAILED = 2;
    static final int BANNER_APPLY = 0, BANNER_SHOW = 1;

    static final String TEXT_LOADING = "Checking the installed game build\u2026";
    static final String TEXT_RESTORE = "Restore original", TEXT_SAVE = "Save";
    static final String TEXT_SAVED = "Saved.", TEXT_SAVING = "Saving\u2026";
    static final String TEXT_GAME_RUNNING = "The game is running. Close it, then save.";
    static final String TEXT_GAME_RUNNING_NOTICE = "The game is running: changes can be saved after it is closed.";
    /** Start of the warning about developer switch files (the same text as SettingsStore.WARNING_STANDING). */
    static final String TEXT_STANDING = "A developer switch file overrules settings shown here: ";
    /** At most this many warnings are shown above the rows, one per line. */
    static final int NOTICE_LINES = 3;
    static final String TEXT_SAVE_FAILED = "Could not save: ";
    static final String TEXT_ONLY_FSR1 = "Only used by FSR 1";
    /** Why the Scaler and FSR Sharpness rows are grey while the render resolution is Native in a stage B build. */
    static final String TEXT_NOT_AT_NATIVE = "Not used at Native";
    static final String TEXT_NEXT_START = "Applies the next time the game starts";
    static final String TEXT_NOT_IN_BUILD = "Not in the installed game build";
    /** The line under the World group's title (lane world-load, SETTINGS-SCREEN.md section 2). */
    static final String TEXT_WORLD_NOTE = "Original = the game as shipped. Lower values are your choice: the game then creates fewer cars and people.";
    static final String TEXT_UNSUPPORTED = " - not in this game build; the game uses ";
    static final String TEXT_NO_FILE = "No saved settings yet: this shows what the game's setting files say. "
            + "Nothing is written until you choose Save.";

    private static final int ROW_RESTORE = 1, ROW_SAVE = 2;
    private static final String GROUP_WORLD = "World";
    private static final String[] RESTORE_BUTTONS = {"Restore", "Cancel"};
    private static final String[] SAVE_BUTTONS = {"Save", "Discard", "Keep editing"};
    private static final String[] DRIFT_BUTTONS = {"Close"};
    private static final String[] BANNER_BUTTONS = {"Apply again", "Show"};

    private static final class Entry {
        final int kind, action, group;
        final SettingSpec spec;      // null for the banner and the two action rows
        final String groupTitle;     // "" for rows without a group title

        Entry(int kind, SettingSpec spec, int action, int group, String groupTitle) {
            this.kind = kind;
            this.spec = spec;
            this.action = action;
            this.group = group;
            this.groupTitle = groupTitle;
        }
    }

    private static final class Line {
        final String text;
        final int tone;

        Line(String text, int tone) {
            this.text = text;
            this.tone = tone;
        }
    }

    private final List<Entry> rows = new ArrayList<>();
    private int phase = PHASE_LOADING;
    private String loadError;
    private Capabilities capabilities;
    private Settings loaded, working;
    private boolean fromFile;
    private List<String> warnings = Collections.emptyList(), drift = Collections.emptyList();
    private int focus;
    private int bannerButton;
    private int overlay = OVERLAY_NONE, overlayFocus;
    private int pending = ACTION_NONE;
    private Settings pendingSettings, savingSettings;
    private boolean saving, leaveAfterSave;
    private String status;
    private int statusTone;
    private boolean gameRunning;

    // ------------------------------------------------------------------------------------------ loading

    /** The saved (or imported) settings and what the installed game build supports are known: build the rows. */
    void loaded(Settings saved, boolean fromFile, List<String> warnings, List<String> drift, Capabilities capabilities) {
        this.loaded = saved;
        this.working = saved;
        this.fromFile = fromFile;
        this.warnings = new ArrayList<>(warnings);
        this.drift = new ArrayList<>(drift);
        this.capabilities = capabilities;
        rows.clear();
        if (!this.drift.isEmpty()) rows.add(new Entry(KIND_BANNER, null, 0, 0, ""));
        int group = 0;
        String title = null;
        for (SettingSpec spec : SettingsSchema.all()) {
            if (!spec.group.equals(title)) {
                group++;
                title = spec.group;
            }
            int kind = spec.kind == SettingSpec.Kind.CHOICE ? KIND_CHOICE
                    : spec.kind == SettingSpec.Kind.SLIDER ? KIND_SLIDER : KIND_TOGGLE;
            rows.add(new Entry(kind, spec, 0, group, title));
        }
        rows.add(new Entry(KIND_ACTION, null, ROW_RESTORE, group + 1, ""));
        rows.add(new Entry(KIND_ACTION, null, ROW_SAVE, group + 1, ""));
        phase = PHASE_READY;
        bannerButton = BANNER_SHOW;
        // The banner can be reached with UP, but it is never where the focus starts: its buttons act on files.
        focus = nextFocusable(rows.get(0).kind == KIND_BANNER ? 0 : -1, 1, false);
    }

    /** Whether the game process existed when the screen last asked (at opening, and at each save). */
    void setGameRunning(boolean running) {
        gameRunning = running;
    }

    void loadFailed(String message) {
        phase = PHASE_FAILED;
        loadError = "Could not read the settings: " + message;
    }

    int phase() {
        return phase;
    }

    /** The one line shown in place of the rows while loading or after a failed load; null once the rows exist. */
    String placeholder() {
        return phase == PHASE_LOADING ? TEXT_LOADING : phase == PHASE_FAILED ? loadError : null;
    }

    // ------------------------------------------------------------------------------------------ commands

    /**
     * A NavInput command. Returns false only for a BACK that leaves the screen (the host then pops it) and for a
     * number that is not a command; every other command is consumed, also when it changes nothing.
     */
    boolean nav(int command, boolean repeat) {
        if (command < NavInput.UP || command > NavInput.ALT) return false;
        if (phase != PHASE_READY) {
            if (command != NavInput.BACK) return true;
            pending = ACTION_LEAVE;
            return false;
        }
        if (pending == ACTION_LEAVE) return command != NavInput.BACK;
        if (saving || pending != ACTION_NONE) return true;
        status = null;
        if (overlay != OVERLAY_NONE) {
            overlayNav(command, repeat);
            return true;
        }
        switch (command) {
            case NavInput.UP: focus = nextFocusable(focus, -1, !repeat); break;
            case NavInput.DOWN: focus = nextFocusable(focus, 1, !repeat); break;
            case NavInput.LEFT: side(-1); break;
            case NavInput.RIGHT: side(1); break;
            case NavInput.CONFIRM: confirm(); break;
            case NavInput.PAGE_PREV: page(-1); break;
            case NavInput.PAGE_NEXT: page(1); break;
            case NavInput.MENU: requestSave(working, false); break;
            case NavInput.ALT: open(OVERLAY_RESTORE, 1); break;
            default:   // BACK
                if (!dirty()) {
                    pending = ACTION_LEAVE;
                    return false;
                }
                // Keep editing first: a player backing out who presses A at once writes nothing and loses nothing
                // (vm-pad-hold D7); Save and Discard are a deliberate step to the left.
                open(OVERLAY_SAVE_QUESTION, SAVE_BUTTONS.length - 1);
        }
        return true;
    }

    /** Touch: a tap on a row focuses it; on a toggle or an action row it also acts like CONFIRM. */
    void tapRow(int index) {
        if (!focusRow(index)) return;
        int kind = rows.get(index).kind;
        if (kind == KIND_TOGGLE || kind == KIND_ACTION) nav(NavInput.CONFIRM, false);
    }

    /** Touch: a tap on the left (direction < 0) or right arrow zone of a row. */
    void tapArrow(int index, int direction) {
        if (focusRow(index)) nav(direction < 0 ? NavInput.LEFT : NavInput.RIGHT, false);
    }

    /** Touch: a tap on one of the banner's two buttons. */
    void tapBannerButton(int button) {
        if (button != BANNER_APPLY && button != BANNER_SHOW) return;
        if (rows.isEmpty() || rows.get(0).kind != KIND_BANNER || !focusRow(0)) return;
        bannerButton = button;
        nav(NavInput.CONFIRM, false);
    }

    /** Touch: a tap on a button of the open question. */
    void tapOverlayButton(int button) {
        if (overlay == OVERLAY_NONE || button < 0 || button >= overlayButtonCount()) return;
        overlayFocus = button;
        nav(NavInput.CONFIRM, false);
    }

    /**
     * The category pages (SettingsPages) choose the highlighted row themselves: focuses a row without acting on it.
     * False, and nothing changes, while a question is open, a save runs or the screen is closing.
     */
    boolean focusOn(int index) {
        return phase == PHASE_READY && focusRow(index);
    }

    /** The setting key of a row; null for the banner and the two action rows. */
    String rowKey(int index) {
        SettingSpec spec = rows.get(index).spec;
        return spec == null ? null : spec.key;
    }

    private boolean focusRow(int index) {
        if (saving || pending != ACTION_NONE || overlay != OVERLAY_NONE) return false;
        if (index < 0 || index >= rows.size()) return false;
        focus = index;
        return true;
    }

    /**
     * The next row. Every row takes the focus, a disabled one too: the list then scrolls to it and its reason can be
     * read with a gamepad. A disabled row is inert: side() and confirm() do nothing on it. With wrap (a fresh press)
     * the step continues at the other end of the list; without it (a held direction) it stops at the end.
     */
    private int nextFocusable(int from, int direction, boolean wrap) {
        int count = rows.size(), index = from + direction;
        if (index >= 0 && index < count) return index;
        return wrap && count > 0 ? (index + count) % count : from;
    }

    /** L1 / R1: the first row of the previous / next group. */
    private void page(int direction) {
        int group = rows.get(focus).group;
        int target = -1;
        if (direction > 0) {
            for (int i = focus + 1; i < rows.size() && target < 0; ++i) {
                if (rows.get(i).group != group) target = i;
            }
        } else {
            int other = -1;   // the group before this one
            for (int i = focus - 1; i >= 0 && other < 0; --i) {
                if (rows.get(i).group != group) other = rows.get(i).group;
            }
            // No earlier group: go to the start of this one.
            int wanted = other < 0 ? group : other;
            for (int i = 0; i < rows.size() && target < 0; ++i) {
                if (rows.get(i).group == wanted) target = i;
            }
        }
        if (target >= 0) focus = target;
    }

    private void side(int direction) {
        Entry row = rows.get(focus);
        if (row.kind == KIND_BANNER) {
            bannerButton = direction < 0 ? BANNER_APPLY : BANNER_SHOW;
            return;
        }
        if (row.spec == null || !rowEnabled(focus)) return;   // a disabled row is inert
        working = working.with(row.spec.key, stepTarget(row.spec, working.get(row.spec.key), direction));
    }

    private void confirm() {
        Entry row = rows.get(focus);
        if (row.kind == KIND_BANNER) {
            if (bannerButton == BANNER_APPLY) requestSave(loaded, false);
            else open(OVERLAY_DRIFT_LIST, 0);
        } else if (row.kind == KIND_ACTION) {
            if (row.action == ROW_RESTORE) open(OVERLAY_RESTORE, 1);
            else requestSave(working, false);
        } else if (row.kind != KIND_SLIDER && rowEnabled(focus)) {
            // (A disabled row is inert here too: the Scaler row at Native has other values, and keeps the saved one.)
            // A toggle has two values and a choice goes round, so both are "the next offered value, wrapping".
            SettingSpec spec = row.spec;
            int value = working.get(spec.key);
            for (int tried = spec.min; tried <= spec.max; tried += spec.step) {
                value = value + spec.step > spec.max ? spec.min : value + spec.step;
                if (capabilities.offered(spec.key, value)) break;
            }
            working = working.with(spec.key, value);
        }
    }

    /** The nearest offered value in one direction; the current value when there is none (stop at the end). */
    private int stepTarget(SettingSpec spec, int from, int direction) {
        for (int value = from + direction * spec.step; value >= spec.min && value <= spec.max; value += direction * spec.step) {
            if (capabilities.offered(spec.key, value)) return value;
        }
        return from;
    }

    private void open(int which, int focusedButton) {
        overlay = which;
        overlayFocus = focusedButton;
    }

    private void overlayNav(int command, boolean repeat) {
        // Only a fresh press moves the highlight of a question: a direction held from before it opened, or held on,
        // must not walk to another answer (menu-input-fixes M2).
        if (repeat && (command == NavInput.LEFT || command == NavInput.RIGHT)) return;
        if (command == NavInput.LEFT) {
            if (overlayFocus > 0) overlayFocus--;
        } else if (command == NavInput.RIGHT) {
            if (overlayFocus < overlayButtonCount() - 1) overlayFocus++;
        } else if (command == NavInput.BACK) {
            overlay = OVERLAY_NONE;   // Cancel, Keep editing or Close
        } else if (command == NavInput.CONFIRM) {
            int which = overlay, button = overlayFocus;
            overlay = OVERLAY_NONE;
            if (which == OVERLAY_RESTORE && button == 0) {
                working = Settings.defaults();
                // The question said "Then choose Save.": put the focus there.
                for (int i = 0; i < rows.size(); ++i) if (rows.get(i).action == ROW_SAVE) focus = i;
            } else if (which == OVERLAY_SAVE_QUESTION && button == 0) {
                requestSave(working, true);
            } else if (which == OVERLAY_SAVE_QUESTION && button == 1) {
                pending = ACTION_LEAVE;   // Discard: nothing is written
            }
        }
    }

    // ------------------------------------------------------------------------------------------ saving

    private void requestSave(Settings settings, boolean leaveAfter) {
        pending = ACTION_SAVE;
        pendingSettings = settings;
        leaveAfterSave = leaveAfter;
    }

    /** NONE, SAVE (call beginSave) or LEAVE (close the screen). */
    int pendingAction() {
        return pending;
    }

    /** Takes the pending save: returns the settings to write. Until saveFinished every command is ignored. */
    Settings beginSave() {
        if (pending != ACTION_SAVE) throw new IllegalStateException("no save is pending");
        pending = ACTION_NONE;
        saving = true;
        savingSettings = pendingSettings;
        pendingSettings = null;
        status = TEXT_SAVING;
        statusTone = TONE_NORMAL;
        return savingSettings;
    }

    /** The result of the write started by beginSave (the fields of SettingsStore.SaveResult). */
    void saveFinished(boolean ok, boolean refusedGameRunning, String error) {
        if (!saving) return;
        saving = false;
        if (ok) {
            loaded = savingSettings;
            fromFile = true;
            // The save settled what the other warnings were about; a developer switch file still stands.
            List<String> kept = new ArrayList<>();
            for (String warning : warnings) if (warning.startsWith(TEXT_STANDING)) kept.add(warning);
            warnings = kept;
            removeBanner();
            gameRunning = error != null;
            status = error == null ? TEXT_SAVED : error;   // error with ok: the game appeared during the save
            statusTone = error == null ? TONE_GOOD : TONE_WARNING;
            if (leaveAfterSave && error == null) pending = ACTION_LEAVE;
        } else if (refusedGameRunning) {
            gameRunning = true;
            status = TEXT_GAME_RUNNING;
            statusTone = TONE_WARNING;
        } else {
            status = TEXT_SAVE_FAILED + (error == null ? "unknown error" : error);
            statusTone = TONE_ERROR;
        }
        savingSettings = null;
    }

    /** After a successful save the files match the saved settings again, so the banner goes. */
    private void removeBanner() {
        drift = Collections.emptyList();
        if (rows.get(0).kind != KIND_BANNER) return;
        rows.remove(0);
        focus = Math.max(0, focus - 1);
    }

    /** True when the edited copy differs from the saved one. */
    boolean dirty() {
        return phase == PHASE_READY && !working.equals(loaded);
    }

    /** Footer status ("Saved.", the game-running line, a save error); null when there is none. */
    String status() {
        return status;
    }

    int statusTone() {
        return statusTone;
    }

    /** The screen calls this two seconds after "Saved." appeared; any other status stays until the next command. */
    void clearSavedStatus() {
        if (TEXT_SAVED.equals(status)) status = null;
    }

    // ------------------------------------------------------------------------------------------ rows

    int rowCount() {
        return rows.size();
    }

    int focused() {
        return focus;
    }

    int rowKind(int index) {
        return rows.get(index).kind;
    }

    /** The group title to print above this row; null when the row does not start a titled group. */
    String rowGroupTitle(int index) {
        Entry row = rows.get(index);
        if (row.groupTitle.isEmpty()) return null;
        if (index > 0 && rows.get(index - 1).group == row.group) return null;
        return row.groupTitle.equals(GROUP_WORLD) ? row.groupTitle + "\n" + TEXT_WORLD_NOTE : row.groupTitle;
    }

    String rowLabel(int index) {
        Entry row = rows.get(index);
        if (row.kind == KIND_BANNER) {
            return "The game's setting files differ from your saved settings (" + drift.size()
                    + (drift.size() == 1 ? " file" : " files") + ").";
        }
        if (row.kind == KIND_ACTION) return row.action == ROW_RESTORE ? TEXT_RESTORE : TEXT_SAVE;
        return row.spec.label;
    }

    /**
     * The value text of a setting row; null for the banner and the action rows. In a stage B build the render
     * resolution is shown as its mode (SettingsSchema.renderModeLabel), otherwise every value as its setting says it.
     */
    String rowValue(int index) {
        SettingSpec spec = rows.get(index).spec;
        if (spec == null) return null;
        int value = working.get(spec.key);
        return stageB(spec) ? SettingsSchema.renderModeLabel(value) : spec.format(value);
    }

    /** True for the Render Resolution row when the installed build has render-resolution stage B. */
    private boolean stageB(SettingSpec spec) {
        return spec.key.equals(SettingsSchema.RENDER_SCALE) && capabilities.renderStageB();
    }

    /**
     * True when the installed build makes frame generation's in-between picture under the automatic frame cap too
     * (Capabilities.generationUnderAutomatic). SettingsPages asks, for its rule between that cap's row and Frame Generation's.
     */
    boolean generationUnderAutomatic() {
        return capabilities.generationUnderAutomatic();
    }

    /**
     * WARNING when the installed build does not support the row's value or the row does not offer it (a saved 50 %
     * or 125 % in a stage B build), and for a World row below Original (a world that is not the original is visible
     * at a glance); DISABLED for a disabled row.
     */
    int rowValueTone(int index) {
        Entry row = rows.get(index);
        if (row.spec == null) return TONE_NORMAL;
        int value = working.get(row.spec.key);
        if (!capabilities.valueSupported(row.spec.key, value)) return TONE_WARNING;
        if (!rowEnabled(index)) return TONE_DISABLED;
        if (!capabilities.offered(row.spec.key, value)) return TONE_WARNING;
        return row.spec.group.equals(GROUP_WORLD) && value != row.spec.defaultValue ? TONE_WARNING : TONE_NORMAL;
    }

    /**
     * The whole help text of a row, one entry per line, for the one row whose help the model writes itself: Render
     * Resolution in a stage B build (what the mode is and what was measured, what the scaler in use does with it,
     * that a change needs a restart). null for every other row: its help is what the row is and rowSecondLine.
     */
    String[] rowHelp(int index) {
        SettingSpec spec = rows.get(index).spec;
        if (spec == null || !stageB(spec)) return null;
        return SettingsSchema.renderModeHelp(working.get(spec.key),
                capabilities.effective(SettingsSchema.SCALER, working.get(SettingsSchema.SCALER)));
    }

    /** The row's second line, or null: why it is disabled, which values are unavailable, or when a change applies. */
    String rowSecondLine(int index) {
        Line line = secondLine(index);
        return line == null ? null : line.text;
    }

    int rowSecondLineTone(int index) {
        Line line = secondLine(index);
        return line == null ? TONE_NORMAL : line.tone;
    }

    /**
     * False for a setting the installed build cannot change (no supported value besides the default, and the row
     * holds a supported value), for FSR sharpness while the scaler in use is not FSR 1, and for the scaler and FSR
     * sharpness while the render resolution is Native in a stage B build. A disabled row can take the focus (so
     * that its reason can be scrolled to and read), ignores every change and keeps its value. The banner and the
     * two action rows are always enabled.
     */
    boolean rowEnabled(int index) {
        SettingSpec spec = rows.get(index).spec;
        if (spec == null) return true;
        // A row holding a value the build does not support stays reachable, so that it alone can be set back.
        boolean canChange = capabilities.anyChoice(spec.key) || !capabilities.valueSupported(spec.key, working.get(spec.key));
        return canChange && !sharpnessUnused(spec) && !unusedAtNative(spec);
    }

    /** True when the row's value differs from the saved one (the dot before the label). */
    boolean rowChanged(int index) {
        SettingSpec spec = rows.get(index).spec;
        return spec != null && working.get(spec.key) != loaded.get(spec.key);
    }

    /**
     * Slider position 0..1, measured from zero and not from the row's lowest value: a density at its floor of 25 %
     * shows a quarter of the bar, not an empty one. -1 for every other kind of row.
     */
    float rowFraction(int index) {
        Entry row = rows.get(index);
        if (row.kind != KIND_SLIDER) return -1f;
        return working.get(row.spec.key) / (float) row.spec.max;
    }

    /** True when LEFT (direction < 0) or RIGHT would change the row's value: the arrow is drawn active. */
    boolean rowCanStep(int index, int direction) {
        Entry row = rows.get(index);
        if (row.spec == null || !rowEnabled(index)) return false;
        int value = working.get(row.spec.key);
        return stepTarget(row.spec, value, direction < 0 ? -1 : 1) != value;
    }

    int bannerButtonCount() {
        return BANNER_BUTTONS.length;
    }

    String bannerButtonLabel(int button) {
        return BANNER_BUTTONS[button];
    }

    int bannerFocusedButton() {
        return bannerButton;
    }

    private boolean sharpnessUnused(SettingSpec spec) {
        if (!spec.key.equals(SettingsSchema.FSR_SHARPNESS)) return false;
        SettingSpec scaler = SettingsSchema.spec(SettingsSchema.SCALER);
        int inUse = capabilities.effective(SettingsSchema.SCALER, working.get(SettingsSchema.SCALER));
        return !"fsr1".equals(scaler.choiceIds[inUse]);
    }

    /**
     * True for the Scaler and FSR Sharpness rows while the render resolution is Native in a stage B build: the game
     * then shows what it drew 1:1, with no scaler and no sharpening.
     */
    private boolean unusedAtNative(SettingSpec spec) {
        if (!spec.key.equals(SettingsSchema.SCALER) && !spec.key.equals(SettingsSchema.FSR_SHARPNESS)) return false;
        return capabilities.renderStageB() && working.get(SettingsSchema.RENDER_SCALE) == SettingsSchema.RENDER_SCALE_150;
    }

    private Line secondLine(int index) {
        Entry row = rows.get(index);
        SettingSpec spec = row.spec;
        if (spec == null) return null;
        int value = working.get(spec.key);
        if (!capabilities.valueSupported(spec.key, value)) {
            return new Line(spec.format(value) + TEXT_UNSUPPORTED + spec.format(capabilities.effective(spec.key, value)), TONE_WARNING);
        }
        if (!capabilities.anyChoice(spec.key)) return new Line(commonReason(spec, false), TONE_DISABLED);
        if (unusedAtNative(spec)) return new Line(TEXT_NOT_AT_NATIVE, TONE_DISABLED);
        if (sharpnessUnused(spec)) return new Line(TEXT_ONLY_FSR1, TONE_DISABLED);
        if (stageB(spec)) return new Line(rowHelp(index)[0], capabilities.offered(spec.key, value) ? TONE_NORMAL : TONE_WARNING);
        // In a stage B build a frame cap's note depends on the render resolution chosen with it (measured per mode).
        String meaning = spec.key.equals(SettingsSchema.FRAME_CAP) && capabilities.renderStageB()
                ? SettingsSchema.capNote(value, working.get(SettingsSchema.RENDER_SCALE)) : SettingsSchema.hint(spec.key, value);
        if (row.kind == KIND_CHOICE && meaning == null) {
            StringBuilder missing = new StringBuilder();
            for (int choice = spec.min; choice <= spec.max; ++choice) {
                if (capabilities.valueSupported(spec.key, choice)) continue;
                missing.append(missing.length() == 0 ? "" : ", ").append(spec.format(choice));
            }
            if (missing.length() > 0) return new Line(commonReason(spec, true) + ": " + missing, TONE_NORMAL);
        }
        if (meaning != null) return new Line(meaning, TONE_NORMAL);
        return rowChanged(index) ? new Line(TEXT_NEXT_START, TONE_NORMAL) : null;
    }

    /**
     * One reason for the values of a setting that the build does not support. For a disabled row it is the reason
     * given for most of them (the first one on a tie). For the list of unavailable values of an enabled row it is
     * their shared reason, or the plain "Not in the installed game build" when they do not share one.
     */
    private String commonReason(SettingSpec spec, boolean shared) {
        List<String> reasons = new ArrayList<>();
        List<Integer> counts = new ArrayList<>();
        for (int value = spec.min; value <= spec.max; value += spec.step) {
            String reason = capabilities.reason(spec.key, value);
            if (reason == null) continue;
            int at = reasons.indexOf(reason);
            if (at < 0) {
                reasons.add(reason);
                counts.add(1);
            } else {
                counts.set(at, counts.get(at) + 1);
            }
        }
        if (shared && reasons.size() > 1) return TEXT_NOT_IN_BUILD;
        int best = 0;
        for (int i = 1; i < reasons.size(); ++i) if (counts.get(i) > counts.get(best)) best = i;
        return reasons.get(best);
    }

    // ------------------------------------------------------------------------------------------ overlay, notice, legend

    int overlay() {
        return overlay;
    }

    int overlayFocused() {
        return overlayFocus;
    }

    int overlayButtonCount() {
        return overlayButtons().length;
    }

    String overlayButtonLabel(int button) {
        return overlayButtons()[button];
    }

    private String[] overlayButtons() {
        return overlay == OVERLAY_RESTORE ? RESTORE_BUTTONS : overlay == OVERLAY_SAVE_QUESTION ? SAVE_BUTTONS
                : overlay == OVERLAY_DRIFT_LIST ? DRIFT_BUTTONS : new String[0];
    }

    String overlayTitle() {
        return overlay == OVERLAY_RESTORE ? "Put every setting back to the original?"
                : overlay == OVERLAY_SAVE_QUESTION ? "Save changes?"
                : overlay == OVERLAY_DRIFT_LIST ? "Setting files that differ from your saved settings" : null;
    }

    /** The text under the question's title, or null. For the file list: one file name per line. */
    String overlayDetail() {
        if (overlay == OVERLAY_RESTORE) return "Then choose Save.";
        if (overlay != OVERLAY_DRIFT_LIST) return null;
        StringBuilder names = new StringBuilder();
        for (String name : drift) names.append(names.length() == 0 ? "" : "\n").append(name);
        return names.toString();
    }

    /**
     * The lines above the rows, or null: that the game is running, then the first NOTICE_LINES warnings (one per
     * line; the loader puts the one about files a Save would rewrite first), or that nothing has been saved yet.
     */
    String notice() {
        if (phase != PHASE_READY) return null;
        StringBuilder text = new StringBuilder(gameRunning ? TEXT_GAME_RUNNING_NOTICE : "");
        for (int i = 0; i < warnings.size() && i < NOTICE_LINES; ++i) {
            text.append(text.length() == 0 ? "" : "\n").append(warnings.get(i));
        }
        if (warnings.size() > NOTICE_LINES) text.append(" (and ").append(warnings.size() - NOTICE_LINES).append(" more)");
        if (text.length() > 0) return text.toString();
        return fromFile ? null : TEXT_NO_FILE;
    }

    int noticeTone() {
        return warnings.isEmpty() && !gameRunning ? TONE_NORMAL : TONE_WARNING;
    }

    /** The footer legend for the current state. Words only: every device font has them. */
    String legend() {
        if (phase != PHASE_READY) return "B Back";
        if (overlay == OVERLAY_DRIFT_LIST) return "A Close   B Close   Up/Down Scroll";
        if (overlay == OVERLAY_RESTORE) return "A Select   B Cancel   Left/Right Move";
        if (overlay == OVERLAY_SAVE_QUESTION) return "A Select   B Keep editing   Left/Right Move";
        if (!rowEnabled(focus)) return "Not available (see the line under the row)   B Back   L1/R1 Group   Y Restore original   Start Save";
        return "A Select   B Back   Left/Right Change   L1/R1 Group   Y Restore original   Start Save";
    }

    // ------------------------------------------------------------------------------------------ helpers for the view

    /**
     * Where the list must be scrolled so that the focused row is fully visible together with its neighbours.
     * All values are pixels in list coordinates. spanTop..spanBottom is the focused row plus one neighbour on each
     * side (the list's start or end for the first or last row); when that is taller than the viewport only the
     * focused row itself (focusTop..focusBottom) is kept visible. The list moves only as far as needed.
     */
    static int scrollOffset(int current, int viewport, int content, int focusTop, int focusBottom, int spanTop, int spanBottom) {
        int top = spanTop, bottom = spanBottom;
        if (bottom - top > viewport) {
            top = focusTop;
            bottom = focusBottom;
        }
        int offset = current;
        if (bottom > offset + viewport) offset = bottom - viewport;
        if (top < offset) offset = top;
        return Math.max(0, Math.min(Math.max(0, content - viewport), offset));
    }

    /** The header's line about the installed game code: size in megabytes (10^6 bytes) and the file's time. */
    static String gameCodeLine(long bytes, long modifiedMs, TimeZone zone) {
        if (bytes <= 0) return "Game code not installed";
        Calendar time = new GregorianCalendar(zone, Locale.ROOT);
        time.setTimeInMillis(modifiedMs);
        return String.format(Locale.ROOT, "Game code: %d MB, %04d-%02d-%02d %02d:%02d", (bytes + 500000) / 1000000,
                time.get(Calendar.YEAR), time.get(Calendar.MONTH) + 1, time.get(Calendar.DAY_OF_MONTH),
                time.get(Calendar.HOUR_OF_DAY), time.get(Calendar.MINUTE));
    }
}
