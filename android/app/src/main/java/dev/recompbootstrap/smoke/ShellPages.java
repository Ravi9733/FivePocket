package dev.recompbootstrap.smoke;

import java.util.List;
import java.util.Locale;
import java.util.TimeZone;

/**
 * What the frame and the Play, Controller and About tabs and the first-run setup page show
 * (local-analysis/agents/app-ui-4/DESIGN.md section 6). Pure Java: it turns the state of LauncherMenu, ShellNav and
 * SetupModel into a MenuScenes.Page and decides nothing itself. The Settings tab is filled by SettingsPages.
 */
final class ShellPages {
    private ShellPages() { }

    /** The title at the top left: the name of the game this private build runs, as plain text. */
    static final String TITLE = "FivePocket"; // owner 2026-10-25 (the project's name; was the game's title)
    static final String TEXT_PRIVATE_BUILD = "Private build", TEXT_NO_CONTROLLER = "No controller";
    static final String[] ABOUT_SECTIONS = {"This App", "Game Build", "Licence Notices", "Graphics Self-Test", CaptureAbout.SECTION};
    static final int ABOUT_APP = 0, ABOUT_BUILD = 1, ABOUT_NOTICES = 2, ABOUT_SELF_TEST = 3, ABOUT_CAPTURES = 4;
    /** What A does on the Captures section while pictures wait to be copied. */
    static final String PROMPT_COPY = "Copy To Pictures";
    static final String TEXT_SELF_TEST = "Opens a graphics test screen. Android's Back returns from it.";
    static final String LABEL_CONTROLLER_CHECK = "Controller Check";
    /** What B does while Play's "start anyway?" question is open. */
    static final String PROMPT_LEAVE_IT = "Leave It";
    static final String TEXT_CONTROLLER_CHECK = "Opens a screen that shows every button, trigger and stick as you use"
            + " it. To leave it, hold Select + Start for one second.";

    private static MenuScenes.Prompt select(String label) {
        return new MenuScenes.Prompt(label, "A", NavInput.CONFIRM);
    }

    private static MenuScenes.Prompt back(String label) {
        return new MenuScenes.Prompt(label, "B", NavInput.BACK);
    }

    /**
     * The game code line for the header: "Game code 2026-10-04 17:26" from LauncherMenu's "Game code: 338 MB,
     * 2026-10-04 17:26" (the size stands on the Play tab), or its words without the colon ("Game code missing").
     */
    static String codeLine(String codeFact) {
        int colon = codeFact.indexOf(": "), comma = codeFact.indexOf(", ");
        if (colon < 0) return codeFact;
        return codeFact.substring(0, colon) + " " + codeFact.substring(comma > colon ? comma + 2 : colon + 2);
    }

    /** The frame every tab shares: the title, the three small lines at the right, the tab strip. */
    static MenuScenes.Page frame(ShellNav nav, LauncherMenu menu, String controllerName, TimeZone zone) {
        MenuScenes.Page page = new MenuScenes.Page();
        page.title = TITLE;
        // The build that is handed out says nothing about what kind of build it is.
        if (!menu.selfInstalling()) page.status.add(new MenuScenes.Line(TEXT_PRIVATE_BUILD, MenuScenes.TONE_NORMAL));
        page.status.add(new MenuScenes.Line(codeLine(menu.codeFact(zone)), MenuScenes.TONE_NORMAL));
        boolean none = noController(controllerName);
        page.status.add(new MenuScenes.Line(none ? TEXT_NO_CONTROLLER : controllerName.trim(), MenuScenes.TONE_NORMAL));
        // Without a controller the prompts are the X and check icons; with one, the strip names its buttons (owner 2026-10-25).
        page.touchIcons = none;
        page.tabs = ShellNav.TAB_NAMES;
        page.tab = nav.tab();
        return page;
    }

    /** True when no controller is connected: the name Android gave is null or empty. */
    static boolean noController(String controllerName) {
        return controllerName == null || controllerName.trim().isEmpty();
    }

    /** "Game data: found" as a row: "Game Data" at the left, "Found" at the right. */
    static MenuScenes.Row fact(String fact) {
        MenuScenes.Row row = new MenuScenes.Row();
        int colon = fact.indexOf(": ");
        row.label = Scene.titleCase(colon < 0 ? fact : fact.substring(0, colon));
        String value = colon < 0 ? "" : fact.substring(colon + 2);
        row.value = value.isEmpty() ? value : Character.toUpperCase(value.charAt(0)) + value.substring(1);
        return row;
    }

    /**
     * After a command or a tap on the Play tab: when it took the highlight off the Play item (up to the tab strip,
     * or another tab is shown now), LauncherMenu is told, which answers an open "start anyway?" question with no.
     * tabBefore is the tab that was shown when the command arrived. A button that does nothing on the tab strip
     * (UP, Start, Y) has taken the highlight nowhere and answers nothing: the question can arrive while the
     * highlight is already on the strip, and there only B (ShellNav.route) and leaving the tab answer it.
     */
    static void afterMove(int tabBefore, ShellNav nav, LauncherMenu menu) {
        if (tabBefore == ShellNav.PLAY && (nav.tab() != ShellNav.PLAY || nav.wentToStrip())) menu.leavePlayItem();
    }

    /** The Play tab: the one item at the left; its state, and the state of game data, game code and controller, at the right. */
    static void play(MenuScenes.Page page, ShellNav nav, LauncherMenu menu, String controllerName, TimeZone zone) {
        boolean onItem = nav.level() != ShellNav.STRIP;
        page.column = new String[] {Scene.titleCase(menu.label(LauncherMenu.PRIMARY))};
        page.columnEnabled = new boolean[] {menu.enabled(LauncherMenu.PRIMARY)};
        page.columnLit = onItem;
        page.info.add(new MenuScenes.Line(menu.stateLine(),
                menu.stateLineIsWarning() ? MenuScenes.TONE_WARNING : MenuScenes.TONE_NORMAL));
        // The whole text about the last attempt (a start failure, the question about unqualified game code), or,
        // when there is none, the short message.
        String failure = menu.failure(), second = menu.secondLine(LauncherMenu.PRIMARY);
        if (failure != null) {
            page.info.add(new MenuScenes.Line(failure, menu.failureIsError() ? MenuScenes.TONE_ERROR : MenuScenes.TONE_WARNING));
        } else if (second != null) {
            page.info.add(new MenuScenes.Line(second, menu.secondLineTone(LauncherMenu.PRIMARY) == LauncherMenu.Tone.ERROR
                    ? MenuScenes.TONE_ERROR : MenuScenes.TONE_NORMAL));
        }
        page.rows.add(fact(menu.dataFact()));
        page.rows.add(fact(menu.codeFact(zone)));
        page.rows.add(fact(LauncherMenu.controllerFact(controllerName)));
        String detail = menu.detail();
        if (detail != null && !detail.isEmpty()) page.help.add(new MenuScenes.Line(detail, MenuScenes.TONE_NORMAL));
        // The game cannot be played without a controller (it shows its own "Please reconnect controller"). Play is
        // still offered: the game's own alert handles a controller that connects later.
        if (noController(controllerName)) page.help.add(new MenuScenes.Line(ShipAdvice.NO_CONTROLLER, MenuScenes.TONE_WARNING));
        if (!onItem) {
            // On the tab strip B does nothing - unless the question is open, which B answers with no from there too.
            if (menu.asking()) page.prompts.add(back(PROMPT_LEAVE_IT));
            page.prompts.add(select(SettingsPages.PROMPT_SELECT));
        } else if (menu.asking()) {
            page.prompts.add(back(PROMPT_LEAVE_IT));
            page.prompts.add(select("Start Anyway"));
        } else {
            page.prompts.add(back(SettingsPages.PROMPT_BACK));
            if (menu.enabled(LauncherMenu.PRIMARY) && !menu.preparing()) page.prompts.add(select(SettingsPages.PROMPT_SELECT));
        }
    }

    /** The Controller tab: the item that opens the controller check, and the controller Android reports. */
    static void controller(MenuScenes.Page page, ShellNav nav, LauncherMenu menu, String controllerName) {
        boolean onItem = nav.level() != ShellNav.STRIP, enabled = menu.enabled(LauncherMenu.CONTROLLER_CHECK);
        page.column = new String[] {LABEL_CONTROLLER_CHECK};
        page.columnEnabled = new boolean[] {enabled};
        page.columnLit = onItem;
        String second = menu.secondLine(LauncherMenu.CONTROLLER_CHECK);
        if (second != null) page.info.add(new MenuScenes.Line(second, MenuScenes.TONE_WARNING));
        // The game's own on-foot controls as a drawn pad over the controller's row (menu-redesign: Settings > Gamepad).
        page.diagram = true;
        page.rows.add(fact(LauncherMenu.controllerFact(controllerName)));
        page.help.add(new MenuScenes.Line(TEXT_CONTROLLER_CHECK, MenuScenes.TONE_NORMAL));
        if (onItem) page.prompts.add(back(SettingsPages.PROMPT_BACK));
        // While a start of the game is being prepared A on the item does nothing (LauncherMenu.confirm), as on Play.
        if (!onItem || (enabled && !menu.preparing())) page.prompts.add(select(SettingsPages.PROMPT_SELECT));
    }

    /** True when an About section's text is longer than the pane, so the highlight can go into it to scroll. */
    static boolean aboutScrollable(int lineCount) {
        return lineCount > MenuLayout.textLinesVisible();
    }

    /** The scroll position of an About text after UP (direction -1) or DOWN (+1): three lines, never past the ends. */
    static int aboutScrolled(int scroll, int lines, int lineCount) {
        return MenuLayout.clampScroll(scroll + lines, lineCount, MenuLayout.textLinesVisible());
    }

    /** The About tab: sections at the left, the selected section's text (already wrapped into lines) at the right. */
    static void about(MenuScenes.Page page, ShellNav nav, List<String> lines, int scroll) {
        about(page, nav, lines, scroll, false);
    }

    /** As above. capturesToCopy: A on the Captures section would copy pictures now (CaptureAbout.canCopy). */
    static void about(MenuScenes.Page page, ShellNav nav, List<String> lines, int scroll, boolean capturesToCopy) {
        int level = nav.level(), section = nav.column(ShellNav.ABOUT);
        page.column = ABOUT_SECTIONS;
        page.columnSelected = section;
        page.columnLit = level != ShellNav.STRIP;
        page.text = lines;
        page.textScroll = scroll;
        page.textFocused = level == ShellNav.PANE;
        if (level != ShellNav.STRIP) page.prompts.add(back(SettingsPages.PROMPT_BACK));
        // A does something on the strip (into the page), on the self-test item, on the Captures section while there
        // are pictures to copy, and on a text long enough to scroll.
        if (level == ShellNav.COLUMN && section == ABOUT_CAPTURES) {
            if (capturesToCopy) page.prompts.add(select(PROMPT_COPY));
        } else if (level == ShellNav.STRIP || (level == ShellNav.COLUMN && (section == ABOUT_SELF_TEST || aboutScrollable(lines.size())))) {
            page.prompts.add(select(SettingsPages.PROMPT_SELECT));
        }
    }

    /**
     * Touch on a step of the first-run setup: a tap on a step that is not highlighted only moves the highlight
     * there (false); a tap on the highlighted one is A (true).
     */
    static boolean setupTap(SetupModel model, int step) {
        if (model.focused() == step) return true;
        model.focus(step);
        return false;
    }

    /** Touch on an answer of the setup's question: the same rule. True when the answer was already highlighted. */
    static boolean setupAnswerTap(SetupModel model, int answer) {
        int count = model.overlayButtons().length;
        if (answer < 0 || answer >= count) return false;
        if (model.overlayFocus() == answer) return true;
        for (int steps = count; steps > 0 && model.overlayFocus() != answer; --steps) {
            model.nav(model.overlayFocus() < answer ? NavInput.RIGHT : NavInput.LEFT, false);
        }
        return false;
    }

    /** The first-run setup page in the same frame: its heading fills the strip, its steps are rows over the whole block. */
    static MenuScenes.Page setup(SetupModel model) {
        MenuScenes.Page page = new MenuScenes.Page();
        page.title = TITLE;
        page.status.add(new MenuScenes.Line(TEXT_PRIVATE_BUILD, MenuScenes.TONE_NORMAL));
        page.tabs = new String[] {model.title().toUpperCase(Locale.ROOT)};
        page.shoulders = false;
        boolean question = model.overlay() != SetupModel.OVERLAY_NONE;
        int focused = model.focused(), working = -1;
        for (int step = 0; step < SetupModel.STEPS; ++step) {
            MenuScenes.Row row = new MenuScenes.Row();
            int state = model.state(step);
            if (state == SetupModel.WORKING) working = step;
            row.label = Scene.titleCase(model.label(step));
            row.value = Scene.titleCase(model.stateWord(step));
            row.enabled = model.actionable(step) || state == SetupModel.WORKING;
            row.valueTone = state == SetupModel.PROBLEM ? MenuScenes.TONE_ERROR
                    : state == SetupModel.DONE ? MenuScenes.TONE_GOOD : MenuScenes.TONE_NORMAL;
            page.rows.add(row);
        }
        if (!question) page.rowFocused = focused;
        // While a step copies or imports: what it is doing, with its bar, above the steps.
        page.infoBar = model.progressFraction();
        if (page.infoBar >= 0f && working >= 0) page.info.add(new MenuScenes.Line(model.detail(working), MenuScenes.TONE_NORMAL));
        String detail = model.detail(focused);
        if (detail != null && (page.info.isEmpty() || focused != working)) {
            page.help.add(new MenuScenes.Line(detail, model.detailIsProblem(focused) ? MenuScenes.TONE_ERROR : MenuScenes.TONE_NORMAL));
        }
        page.prompts.addAll(setupPrompts(model));
        if (question) page.alert = new ConfirmOverlay(model.overlayText(), null, model.overlayButtons(), model.overlayFocus());
        return page;
    }

    /**
     * The setup page's prompts are the ones its model's footer names. SetupModel.footer names only what a button
     * does right now: A on a step that A would start, B always (it leaves the page, also while the chosen disc is
     * being read, or asks whether to stop the copy), and both under a question. So nothing is taken away here;
     * NavContractTest holds the prompts of every state against what the buttons then do.
     */
    private static List<MenuScenes.Prompt> setupPrompts(SetupModel model) {
        return MenuScenes.legendPrompts(model.footer());
    }
}
