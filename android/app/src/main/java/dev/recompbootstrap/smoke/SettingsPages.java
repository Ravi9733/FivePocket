package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;

/**
 * The settings page as categories (local-analysis/agents/app-ui-4/DESIGN.md section 5): which of MenuModel's rows
 * belongs to which category and in which order, how UP / DOWN move inside a category, what a tap does, what the
 * info block and the help strip say, and which button prompts stand. Pure Java. It decides nothing about values or
 * files: every change still goes through MenuModel, whose row list it only regroups. It holds two rules between rows:
 * Frame Generation is unavailable unless the frame cap is 30 fps - or Automatic, in a build that makes its picture
 * there (blocked, orderNote) -, and the Frame-Rate Counter row says what a frame cap below 60 fps does to the counter
 * (counterNote).
 */
final class SettingsPages {
    static final int DISPLAY = 0, FRAME_RATE = 1, WORLD = 2, CONTROLS = 3, SAVE = 4, COUNT = 5;
    static final String[] TITLES = {"Display", "Frame Rate", "World", "Controls", "Save And Restore"};

    /**
     * Which settings each category holds. Inside a category the rows stand in the settings schema's order. The rows
     * without a setting key (the "files differ" row and the two action rows) are SAVE's.
     */
    private static final String[][] ORDER = {
            {SettingsSchema.SCALER, SettingsSchema.FSR_SHARPNESS, SettingsSchema.RENDER_SCALE},
            {SettingsSchema.FRAME_CAP, SettingsSchema.FRAME_GENERATION, SettingsSchema.FPS_OVERLAY},
            {SettingsSchema.WORLD_TRAFFIC, SettingsSchema.WORLD_PEDESTRIANS, SettingsSchema.WORLD_PARKED_CARS,
                    SettingsSchema.WORLD_SCENARIO_PEDESTRIANS, SettingsSchema.WORLD_DRAW_DISTANCE},
            {SettingsSchema.INPUT_UNBUFFERED},
            {}};

    static final String LABEL_DRIFT = "Setting Files Differ";
    static final String TEXT_DRIFT_HELP = "Apply Again writes your saved settings to the game's files. Show lists what differs.";
    static final String TEXT_RESTORE_HELP = "Puts every row back to the original game's value. Nothing is written until you save.";
    static final String TEXT_SAVE_HELP = "Writes the settings to the game's files. They apply the next time the game starts.";
    static final String TEXT_NOT_AVAILABLE = "Not available: ";
    /** In place of "Not available: Not in this version" (setup-fixes, defect 8 of the vm-robust run). */
    static final String TEXT_NOT_IN_VERSION = "Not available in this version.";
    /** Why Frame Generation cannot be used in a build that has it (after TEXT_NOT_AVAILABLE): see blocked(). */
    static final String TEXT_NEEDS_CAP_30 = "Needs Frame Rate Cap at 30 fps";
    /** The same reason in a build that makes In-Between Frames under the automatic cap too (MenuModel.generationUnderAutomatic). */
    static final String TEXT_NEEDS_CAP_30_OR_AUTO = "Needs Frame Rate Cap at 30 fps or Automatic";
    /** Under the Frame Generation row of such a build while Predicted Frames is chosen under the automatic cap: see orderNote(). */
    static final String TEXT_PREDICTED_NEEDS_CAP_30 = SettingsSchema.spec(SettingsSchema.FRAME_GENERATION).format(SettingsSchema.FG_AHEAD)
            + " needs Frame Rate Cap at 30 fps";
    /** What the Frame Generation row is in such a build: explain()'s sentence with both caps (see describe()). */
    static final String TEXT_GENERATION_30_OR_AUTO = "Adds a generated picture to every frame the game draws under the 30 fps or Automatic cap.";
    /** Under the Frame-Rate Counter row while the frame cap is not 60 fps: see counterNote(). */
    static final String TEXT_COUNTER_UNDER_CAP = "With a frame cap below 60 fps the counter may not appear in this game version.";
    static final String TEXT_SEE_SAVE = " See " + TITLES[SAVE] + ".";
    /**
     * MenuModel's line under the Frame Rate Cap row when the one value the build cannot honour is "Off". The row never
     * offers Off, so the line would explain the absence of a choice nobody was shown: the help strip leaves it out.
     * (A build that lacks another cap too gets the model's list of all of them, as before.)
     */
    static final String TEXT_ONLY_OFF_MISSING = Capabilities.REASON_NO_UNCAPPED + ": "
            + SettingsSchema.spec(SettingsSchema.FRAME_CAP).format(SettingsSchema.CAP_OFF);
    static final String PROMPT_SELECT = "Select", PROMPT_BACK = "Back";
    /** What the screen says, after MenuModel's "Could not read the settings: " or "Could not save: ", when reading or writing threw. */
    static final String TEXT_FAILURE = "something went wrong with the files. Open this tab again to try once more.";

    private static final String[] CATEGORY_HELP = {
            "How the game picture is enlarged and sharpened for this screen.",
            "How many frames the game shows each second, and whether generated pictures are added.",
            MenuModel.TEXT_WORLD_NOTE,
            "How controller input reaches the game.",
            "Save writes your choices to the game's files. Restore Original puts every row back to the original game's value."};

    /** The remembered row (its place inside the category) per category. */
    private final int[] slot = new int[COUNT];

    // ------------------------------------------------------------------------------------------ grouping

    /** The category of a setting. A key this class does not list goes by its schema group, so no row can be lost. */
    static int categoryOfKey(String key) {
        if (key == null) return SAVE;
        for (int category = 0; category < COUNT; ++category) {
            for (String listed : ORDER[category]) if (listed.equals(key)) return category;
        }
        String group = SettingsSchema.spec(key).group;
        return group.equals("World") ? WORLD : group.equals("Controller") ? CONTROLS : DISPLAY;
    }

    /** MenuModel's row numbers of one category, top to bottom; empty while the model has no rows. */
    static int[] rows(MenuModel model, int category) {
        List<Integer> found = new ArrayList<Integer>();
        for (int index = 0; index < model.rowCount(); ++index) {
            if (categoryOfKey(model.rowKey(index)) == category) found.add(index);
        }
        int[] rows = new int[found.size()];
        for (int i = 0; i < rows.length; ++i) rows[i] = found.get(i);
        return rows;
    }

    /** The category the model's focused row is in. */
    static int categoryOfFocus(MenuModel model) {
        return categoryOfKey(model.rowKey(model.focused()));
    }

    /** The place of the model's focused row inside its category. */
    static int slotOfFocus(MenuModel model) {
        int[] rows = rows(model, categoryOfFocus(model));
        for (int i = 0; i < rows.length; ++i) if (rows[i] == model.focused()) return i;
        return 0;
    }

    /**
     * The line for a failure nobody planned for (an exception while the files were read or written): first what
     * happened and what to do, in plain words, then the technical cause in brackets for whoever is asked for help.
     */
    static String failure(Exception problem) {
        String message = problem.getMessage();
        return TEXT_FAILURE + " (" + problem.getClass().getSimpleName() + (message == null ? "" : ": " + message) + ")";
    }

    // ------------------------------------------------------------------------------------------ a rule between rows

    /**
     * Why a row that the installed game build has cannot be used with the other rows as they stand, or null. One
     * rule: frame generation makes its second picture only under the fixed 30 fps frame cap
     * (SettingsSchema.generationPaced; RuntimeMapping writes its switch file under no other cap), so under any other
     * cap the Frame Generation row is unavailable: drawn grey, inert to LEFT, RIGHT, A and taps, with this reason in
     * the help strip. It keeps its value, which returns with the 30 fps cap. A row the model itself disables gives the
     * model's reason, and a row holding a value the build does not have stays usable so that it can be put back.
     * In a build that makes In-Between Frames under the automatic cap too (Capabilities.generationPaced) the row is
     * usable under that cap as well - from Off, or that order could never be chosen there - and the reason under the
     * other caps names both. What Predicted Frames needs there is orderNote's to say.
     */
    static String blocked(MenuModel model, int index) {
        if (!SettingsSchema.FRAME_GENERATION.equals(model.rowKey(index))) return null;
        if (!model.rowEnabled(index) || model.rowValueTone(index) == MenuModel.TONE_WARNING) return null;
        int cap = shown(model, SettingsSchema.FRAME_CAP);
        if (SettingsSchema.generationPaced(cap)) return null;
        if (!model.generationUnderAutomatic()) return TEXT_NEEDS_CAP_30;
        return cap == SettingsSchema.CAP_AUTO ? null : TEXT_NEEDS_CAP_30_OR_AUTO;
    }

    /**
     * What the Frame Generation row must add, while it is usable, because of the frame cap chosen with it, or null.
     * Asked only for a row that blocked() lets through. One case: Predicted Frames, an order the build has, under the
     * automatic cap. The row is usable there only in a build that makes In-Between Frames under that cap, and Predicted
     * Frames still needs the fixed 30 fps cap (RuntimeMapping plans its switch file absent): the row says so. It is
     * not made unavailable for it: a row that turned inert on the order it was just stepped to could not be stepped
     * back. It keeps the order, which returns with the 30 fps cap. A row holding an order the build does not have
     * gives the model's line about that instead.
     */
    private static String orderNote(MenuModel model, int index) {
        if (!SettingsSchema.FRAME_GENERATION.equals(model.rowKey(index)) || model.rowValueTone(index) == MenuModel.TONE_WARNING) return null;
        return shown(model, SettingsSchema.FRAME_CAP) == SettingsSchema.CAP_AUTO
                && shown(model, SettingsSchema.FRAME_GENERATION) == SettingsSchema.FG_AHEAD ? TEXT_PREDICTED_NEEDS_CAP_30 : null;
    }

    /**
     * What the Frame-Rate Counter row must add because of the frame cap chosen with it, or null. Game build v464 draws the
     * counter into the top right corner of the picture (generic_renderer.cpp, the block after the frame's blit). On the
     * phone it was there with the 60 fps cap and missing with the 30 fps cap, same build and same switch file (the lead's
     * two runs of 2026-10-05; docs/fable/DISCOVERIES.md 19:37). Why is open, and 40 fps, 24 fps and Automatic - which
     * pace the game through the same files as 30 fps - were not looked at: so the row promises the counter at 60 fps
     * only (SettingsSchema.counterSeenUnder) and says so under every other cap. A row the build does not have gives the
     * model's reason instead.
     */
    static String counterNote(MenuModel model, int index) {
        if (!SettingsSchema.FPS_OVERLAY.equals(model.rowKey(index)) || !model.rowEnabled(index)) return null;
        int cap = shown(model, SettingsSchema.FRAME_CAP);
        return cap < 0 || SettingsSchema.counterSeenUnder(cap) ? null : TEXT_COUNTER_UNDER_CAP;
    }

    /** The value a setting's row shows now (the edited one), read back from the row's text; -1 when it cannot be told. */
    private static int shown(MenuModel model, String key) {
        SettingSpec spec = SettingsSchema.spec(key);
        for (int index = 0; index < model.rowCount(); ++index) {
            if (!key.equals(model.rowKey(index))) continue;
            String text = model.rowValue(index);
            for (int value = spec.min; value <= spec.max; value += spec.step) {
                if (spec.format(value).equals(text)) return value;
            }
        }
        return -1;
    }

    /** True for the commands that change the highlighted row's value. */
    private static boolean changes(int command) {
        return command == NavInput.LEFT || command == NavInput.RIGHT || command == NavInput.CONFIRM;
    }

    // ------------------------------------------------------------------------------------------ moving

    int slot(int category) {
        return slot[category];
    }

    /** The highlight goes into a category: to the row it was on last time, the first row otherwise. False when it cannot. */
    boolean enter(MenuModel model, int category) {
        int[] rows = rows(model, category);
        if (rows.length == 0) return false;
        slot[category] = Math.max(0, Math.min(slot[category], rows.length - 1));
        return model.focusOn(rows[slot[category]]);
    }

    /**
     * A freshly read model has taken the old one's place (the tab was opened again, the app came back to the front).
     * A highlight that was in the rows returns to the row it was on - also while another tab is shown, so that the
     * model's focused row and the remembered place never disagree. Without rows it goes to the category's name.
     */
    void reloaded(MenuModel model, ShellNav nav) {
        int category = nav.column(ShellNav.SETTINGS);
        if (nav.inPane(ShellNav.SETTINGS) && !enter(model, category)) nav.place(ShellNav.SETTINGS, category, false);
    }

    /**
     * UP (direction -1) or DOWN (+1) inside the focused row's category. A fresh press wraps from the last row to the
     * first and back; a held direction stops at the end. Returns true when the highlight moved.
     */
    boolean move(MenuModel model, int direction, boolean repeat) {
        int category = categoryOfFocus(model);
        int[] rows = rows(model, category);
        int next = slotOfFocus(model) + direction;
        if (next < 0 || next >= rows.length) {
            if (repeat || rows.length < 2) return false;
            next = next < 0 ? rows.length - 1 : 0;
        }
        if (!model.focusOn(rows[next])) return false;
        slot[category] = next;
        return true;
    }

    /** The model put the highlight on a row itself (a question that ends on Save): remember that place. */
    void adopt(MenuModel model) {
        slot[categoryOfFocus(model)] = slotOfFocus(model);
    }

    /** The model's focused row is where the highlight is now: the frame follows it into that row's category. */
    void follow(MenuModel model, ShellNav nav) {
        if (model.placeholder() != null) return;
        adopt(model);
        nav.intoPane(ShellNav.SETTINGS, categoryOfFocus(model));
    }

    /**
     * Gives a command to the model. When the model answers by putting its focus on a row itself - "Restore original"
     * was confirmed and ends on the Save row, or the focus is on another row than before - the highlight follows it
     * there, from wherever it was (a category's name, the tab strip).
     */
    void forward(MenuModel model, ShellNav nav, int command, boolean repeat) {
        // A row this page holds unavailable is as inert as one the model disables. (Under a question the command is the question's.)
        if (changes(command) && nav.level() == ShellNav.PANE && model.placeholder() == null
                && model.overlay() == MenuModel.OVERLAY_NONE && blocked(model, model.focused()) != null) return;
        int before = model.focused();
        boolean restore = command == NavInput.CONFIRM && model.overlay() == MenuModel.OVERLAY_RESTORE && model.overlayFocused() == 0;
        model.nav(command, repeat);
        if (model.placeholder() == null && (model.focused() != before || restore)) follow(model, nav);
    }

    /** True when the highlight is on this row of the model: in the rows, and the model's focus is this row. */
    private static boolean highlighted(MenuModel model, ShellNav nav, int row) {
        return nav.tab() == ShellNav.SETTINGS && nav.level() == ShellNav.PANE && model.focused() == row;
    }

    /**
     * Touch on the rows of the shown category. base is MenuLayout.HIT_ROW, HIT_ARROW_LEFT or HIT_ARROW_RIGHT, slot
     * the row's place in the category. The rows are slim, so a tap on a row that is not highlighted only moves the
     * highlight there. A tap on the highlighted row acts: on its left or right half it steps the value (a choice, a
     * slider, the two answers of the "files differ" row), on a switch or an action row it is A. Returns false when
     * there is no such row.
     */
    boolean tap(MenuModel model, ShellNav nav, int base, int slotTapped) {
        int[] rows = rows(model, nav.column(ShellNav.SETTINGS));
        if (slotTapped < 0 || slotTapped >= rows.length) return false;
        int row = rows[slotTapped];
        if (!highlighted(model, nav, row)) {
            if (model.focusOn(row)) follow(model, nav);
        } else if (blocked(model, row) != null) {
            return true;   // unavailable here: the tap is the row's, and does nothing
        } else if (base == MenuLayout.HIT_ARROW_LEFT || base == MenuLayout.HIT_ARROW_RIGHT) {
            model.tapArrow(row, base == MenuLayout.HIT_ARROW_LEFT ? -1 : 1);
        } else {
            model.tapRow(row);   // A on a switch or an action row; nothing on the others
        }
        return true;
    }

    /**
     * Touch on an answer of the open question: a tap on an answer that is not highlighted only moves the highlight
     * to it; a tap on the highlighted one chooses it, as A does.
     */
    void tapAnswer(MenuModel model, ShellNav nav, int answer) {
        if (model.overlay() == MenuModel.OVERLAY_NONE || answer < 0 || answer >= model.overlayButtonCount()) return;
        if (model.overlayFocused() == answer) {
            forward(model, nav, NavInput.CONFIRM, false);
            return;
        }
        for (int steps = model.overlayButtonCount(); steps > 0 && model.overlayFocused() != answer; --steps) {
            model.nav(model.overlayFocused() < answer ? NavInput.RIGHT : NavInput.LEFT, false);
        }
    }

    /** True when the model's focused row is the "files differ" row (a save started from there removes that row). */
    static boolean onDriftRow(MenuModel model) {
        return model.rowKind(model.focused()) == MenuModel.KIND_BANNER;
    }

    /**
     * A save has finished. wasOnDriftRow: onDriftRow() when it started. While the highlight is on a category's name
     * it stays there. In the rows: if the "files differ" row it was on is gone now, it goes back to the category's
     * name; otherwise it stays on the row it was on (whose number may have changed).
     */
    void afterSave(MenuModel model, ShellNav nav, boolean wasOnDriftRow) {
        if (nav.level() != ShellNav.PANE) return;
        if (wasOnDriftRow && model.rowKind(0) != MenuModel.KIND_BANNER) nav.place(ShellNav.SETTINGS, SAVE, false);
        else follow(model, nav);
    }

    // ------------------------------------------------------------------------------------------ what is drawn

    /** Fills in the settings tab: the column, the notices, the shown category's rows, the help, the prompts, the question. */
    void fill(MenuScenes.Page page, MenuModel model, ShellNav nav, int alertScroll) {
        int level = nav.level(), category = nav.column(ShellNav.SETTINGS);
        boolean ready = model.placeholder() == null, inRows = ready && level == ShellNav.PANE;
        page.column = TITLES;
        page.columnSelected = category;
        // Also while the rows are not there yet (the files are being read again): the highlight is never nowhere.
        page.columnLit = level != ShellNav.STRIP;
        if (ready) {
            page.columnMarks = new int[COUNT];
            for (int i = 0; i < COUNT; ++i) page.columnMarks[i] = mark(model, i);
            for (int row : rows(model, category)) page.rows.add(row(model, row));
            if (inRows) page.rowFocused = slotOfFocus(model);
            MenuScenes.Line notice = notice(model);
            if (notice != null) page.info.add(notice);
            page.help.addAll(inRows ? describe(model, model.focused()) : describeCategory(category));
        } else {
            page.info.add(new MenuScenes.Line(model.placeholder(),
                    model.phase() == MenuModel.PHASE_FAILED ? MenuScenes.TONE_ERROR : MenuScenes.TONE_NORMAL));
        }
        page.prompts.addAll(prompts(model, level, category));
        String status = model.status();
        if (status != null) {
            int tone = model.statusTone();
            page.footerStatus = new MenuScenes.Line(status, tone == MenuModel.TONE_WARNING ? MenuScenes.TONE_WARNING
                    : tone == MenuModel.TONE_ERROR ? MenuScenes.TONE_ERROR
                    : tone == MenuModel.TONE_GOOD ? MenuScenes.TONE_GOOD : MenuScenes.TONE_NORMAL);
        }
        if (model.overlay() != MenuModel.OVERLAY_NONE) {
            String[] labels = new String[model.overlayButtonCount()];
            for (int b = 0; b < labels.length; ++b) labels[b] = model.overlayButtonLabel(b);
            page.alert = new ConfirmOverlay(model.overlayTitle(), model.overlayDetail(), labels, model.overlayFocused());
            page.alert.scroll = alertScroll;
        }
    }

    /** The mark at a category's name: a warning for the "files differ" row, else whether one of its rows was changed. */
    static int mark(MenuModel model, int category) {
        int mark = MenuScenes.MARK_NONE;
        for (int index : rows(model, category)) {
            if (model.rowKind(index) == MenuModel.KIND_BANNER) return MenuScenes.MARK_WARNING;
            if (model.rowChanged(index)) mark = MenuScenes.MARK_CHANGED;
        }
        return mark;
    }

    static MenuScenes.Row row(MenuModel model, int index) {
        MenuScenes.Row row = new MenuScenes.Row();
        int kind = model.rowKind(index);
        if (kind == MenuModel.KIND_BANNER) {
            int button = model.bannerFocusedButton();
            row.label = LABEL_DRIFT;
            row.value = Scene.titleCase(model.bannerButtonLabel(button));
            row.arrows = row.halves = true;
            row.canLeft = button != MenuModel.BANNER_APPLY;
            row.canRight = button != MenuModel.BANNER_SHOW;
            row.valueTone = MenuScenes.TONE_WARNING;
            return row;
        }
        row.label = Scene.titleCase(model.rowLabel(index));
        row.value = model.rowValue(index);
        row.arrows = row.value != null;
        row.halves = kind == MenuModel.KIND_CHOICE || kind == MenuModel.KIND_SLIDER;
        boolean usable = blocked(model, index) == null;
        row.canLeft = usable && model.rowCanStep(index, -1);
        row.canRight = usable && model.rowCanStep(index, 1);
        row.enabled = usable && model.rowEnabled(index);
        row.marked = model.rowChanged(index);
        row.fraction = model.rowFraction(index);
        row.valueTone = model.rowValueTone(index) == MenuModel.TONE_WARNING ? MenuScenes.TONE_WARNING : MenuScenes.TONE_NORMAL;
        return row;
    }

    /**
     * One sentence that says what a setting is, short enough for one line of the help strip (a test measures it):
     * the row's description, as the game's own settings pages give one under their rows. For the four density
     * sliders it is the sentence MenuModel shows for a value below Original, so that the help strip never says the
     * same thing twice in other words.
     */
    static String explain(String key) {
        switch (key) {
            case SettingsSchema.FRAME_CAP: return "The most frames the game shows each second. A lower cap runs cooler and steadier.";
            case SettingsSchema.SCALER: return "How the game picture is enlarged to fill this screen.";
            case SettingsSchema.FSR_SHARPNESS: return "How strongly FSR 1 sharpens the enlarged picture.";
            case SettingsSchema.RENDER_SCALE: return "The size the game draws at before the picture is enlarged.";
            case SettingsSchema.FRAME_GENERATION: return "Adds a generated picture to every frame the game draws under the 30 fps cap.";
            case SettingsSchema.FPS_OVERLAY: return "Shows the frame rate in the top right corner while the game runs.";
            case SettingsSchema.WORLD_TRAFFIC:
            case SettingsSchema.WORLD_PEDESTRIANS:
            case SettingsSchema.WORLD_PARKED_CARS:
            case SettingsSchema.WORLD_SCENARIO_PEDESTRIANS: return SettingsSchema.hint(key, SettingsSchema.WORLD_DENSITY_MIN);
            case SettingsSchema.WORLD_DRAW_DISTANCE: return "How far away the game draws detailed objects.";
            case SettingsSchema.INPUT_UNBUFFERED: return "Passes stick movement to the game as soon as Android reports it.";
            default: return SettingsSchema.spec(key).label + ".";
        }
    }

    /**
     * The help strip for the highlighted row: what the row is, then what MenuModel has to say about it - the
     * reason it cannot be changed, a measured note, a value this build does not support, or when a change applies.
     * Where the model writes the whole help itself (MenuModel.rowHelp), the strip is that: its first line in the
     * warning colour when the model's second line is a warning.
     */
    static List<MenuScenes.Line> describe(MenuModel model, int index) {
        List<MenuScenes.Line> lines = new ArrayList<MenuScenes.Line>();
        String key = model.rowKey(index);
        if (key == null) {
            if (model.rowKind(index) == MenuModel.KIND_BANNER) {
                lines.add(new MenuScenes.Line(TEXT_DRIFT_HELP, MenuScenes.TONE_NORMAL));
            } else {
                boolean save = MenuModel.TEXT_SAVE.equals(model.rowLabel(index));
                lines.add(new MenuScenes.Line(save ? TEXT_SAVE_HELP : TEXT_RESTORE_HELP, MenuScenes.TONE_NORMAL));
            }
            return lines;
        }
        String[] whole = model.rowHelp(index);
        if (whole != null) {
            boolean warning = model.rowSecondLineTone(index) == MenuModel.TONE_WARNING;
            for (int i = 0; i < whole.length; ++i) {
                lines.add(new MenuScenes.Line(whole[i], i == 0 && warning ? MenuScenes.TONE_WARNING : MenuScenes.TONE_NORMAL));
            }
            return lines;
        }
        // (Frame Generation's sentence names the caps it works under: both of them in a build that has both.)
        String what = SettingsSchema.FRAME_GENERATION.equals(key) && model.generationUnderAutomatic() ? TEXT_GENERATION_30_OR_AUTO : explain(key);
        lines.add(new MenuScenes.Line(what, MenuScenes.TONE_NORMAL));
        String blocked = blocked(model, index);
        if (blocked != null) {
            lines.add(new MenuScenes.Line(notAvailable(blocked), MenuScenes.TONE_NORMAL));
            return lines;
        }
        String counter = counterNote(model, index);
        if (counter != null) lines.add(new MenuScenes.Line(counter, MenuScenes.TONE_NORMAL));
        String order = orderNote(model, index);
        if (order != null) lines.add(new MenuScenes.Line(order, MenuScenes.TONE_NORMAL));
        String second = model.rowSecondLine(index);
        if (second != null && !second.equals(what) && !second.equals(TEXT_ONLY_OFF_MISSING)) {
            int tone = model.rowSecondLineTone(index);
            lines.add(new MenuScenes.Line(tone == MenuModel.TONE_DISABLED ? notAvailable(second) : second,
                    tone == MenuModel.TONE_WARNING ? MenuScenes.TONE_WARNING : MenuScenes.TONE_NORMAL));
        }
        return lines;
    }

    /** "Not available: (reason)", or one sentence when the reason is only that this version lacks it. */
    static String notAvailable(String reason) {
        return Capabilities.REASON_NOT_BUILT.equals(reason) || Capabilities.REASON_LOD_LOCKED.equals(reason) ? TEXT_NOT_IN_VERSION
                : TEXT_NOT_AVAILABLE + reason;
    }

    /** The help strip while the highlight is on a category's name: what the category holds. */
    static List<MenuScenes.Line> describeCategory(int category) {
        List<MenuScenes.Line> lines = new ArrayList<MenuScenes.Line>();
        lines.add(new MenuScenes.Line(CATEGORY_HELP[category], MenuScenes.TONE_NORMAL));
        return lines;
    }

    /**
     * The info block above the rows, or null: that the game's files differ from the saved settings (with where to
     * act on it), then MenuModel's notice, whole.
     */
    static MenuScenes.Line notice(MenuModel model) {
        if (model.placeholder() != null) return null;
        String text = model.notice();
        int tone = model.noticeTone() == MenuModel.TONE_WARNING ? MenuScenes.TONE_WARNING : MenuScenes.TONE_NORMAL;
        if (model.rowCount() > 0 && model.rowKind(0) == MenuModel.KIND_BANNER) {
            text = model.rowLabel(0) + TEXT_SEE_SAVE + (text == null || text.equals(MenuModel.TEXT_NO_FILE) ? "" : "\n" + text);
            tone = MenuScenes.TONE_WARNING;
        }
        return text == null ? null : new MenuScenes.Line(text, tone);
    }

    /**
     * The button prompts for the settings tab, leftmost first; A is the rightmost. level is ShellNav.STRIP, COLUMN
     * or PANE, category the shown one. A prompt stands only where its button does something.
     */
    static List<MenuScenes.Prompt> prompts(MenuModel model, int level, int category) {
        List<MenuScenes.Prompt> prompts = new ArrayList<MenuScenes.Prompt>();
        // While a save is being written the model ignores every command (MenuModel.nav), so no button is named.
        if (MenuModel.TEXT_SAVING.equals(model.status())) return prompts;
        int overlay = model.overlay();
        if (overlay != MenuModel.OVERLAY_NONE) {
            prompts.add(new MenuScenes.Prompt(overlay == MenuModel.OVERLAY_RESTORE ? "Cancel"
                    : overlay == MenuModel.OVERLAY_SAVE_QUESTION ? "Keep Editing" : "Close", "B", NavInput.BACK));
            prompts.add(new MenuScenes.Prompt(PROMPT_SELECT, "A", NavInput.CONFIRM));
            return prompts;
        }
        boolean ready = model.placeholder() == null;
        if (ready) {
            prompts.add(new MenuScenes.Prompt(MenuModel.TEXT_SAVE, "START", NavInput.MENU));
            prompts.add(new MenuScenes.Prompt(Scene.titleCase(MenuModel.TEXT_RESTORE), "Y", NavInput.ALT));
        }
        if (level != ShellNav.STRIP) prompts.add(new MenuScenes.Prompt(PROMPT_BACK, "B", NavInput.BACK));
        boolean select;
        if (level == ShellNav.PANE && ready) {
            int index = model.focused();
            select = model.rowEnabled(index) && blocked(model, index) == null && model.rowKind(index) != MenuModel.KIND_SLIDER;
        } else {
            // On the strip A goes into the page; on a category's name it goes into its rows, once they exist.
            select = level == ShellNav.STRIP || (ready && rows(model, category).length > 0);
        }
        if (select) prompts.add(new MenuScenes.Prompt(PROMPT_SELECT, "A", NavInput.CONFIRM));
        return prompts;
    }
}
