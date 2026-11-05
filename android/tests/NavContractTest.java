package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.begin;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.end;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.File;
import java.util.List;
import java.util.TimeZone;

/**
 * Host test of the navigation contract (local-analysis/agents/ship-week/menu-nav/NAV-CONTRACT.md) where a rule is
 * kept by two classes together: the button prompts ShellPages puts on a page against what the button then does in
 * the model that decides it (SetupModel, LauncherMenu, ShellNav). The rule under test is the contract's P-1: a prompt
 * stands exactly where its button does something. Run with java -ea.
 *
 * The rules of one class alone are in that class's own test (ShellNavTest, NavInputTest, MenuModelTest,
 * SetupModelTest, SettingsPagesTest); nothing of those is repeated here.
 */
public final class NavContractTest {
    private static final TimeZone UTC = TimeZone.getTimeZone("UTC");
    private static final int UP = NavInput.UP, LEFT = NavInput.LEFT, A = NavInput.CONFIRM, B = NavInput.BACK,
            START = NavInput.MENU, Y = NavInput.ALT;
    private static final long GB = 1000000000L;

    private interface Make { SetupModel model(); }

    private static MenuScenes.Prompt prompt(List<MenuScenes.Prompt> prompts, int command) {
        for (MenuScenes.Prompt prompt : prompts) if (prompt.command == command) return prompt;
        return null;
    }

    // ---- the first-run setup page

    private static SetupModel.Disk disk(InstallGuard.Existing existing, int present, int total, boolean bundle) {
        return new SetupModel.Disk(existing, present, total, (long) (total - present) * GB, 20 * GB, bundle, false);
    }

    private static SetupModel fresh() {
        SetupModel model = new SetupModel();
        model.setDisk(disk(null, 0, 10, false));
        return model;
    }

    private static SetupModel recognised() {
        SetupModel model = fresh();
        model.identifyStarted();
        model.playDiscIdentified(new DiscIdentifier.Result(DiscIdentifier.Kind.SUPPORTED_PLAY_DISC, "ab", "Recognised.", null));
        return model;
    }

    private static SetupModel copied(boolean bundle) {
        SetupModel model = new SetupModel();
        model.setDisk(disk(null, 10, 10, bundle));
        return model;
    }

    /**
     * True when the button does something on this model: it leaves the page (an unhandled B), asks the screen for
     * an action, opens or closes a question, or moves a highlight.
     */
    private static boolean acts(SetupModel model, int command) {
        String before = model.focused() + "/" + model.overlay() + "/" + model.overlayFocus();
        boolean handled = model.nav(command, false);
        int action = model.takeAction();
        return !handled || action != SetupModel.ACT_NONE || !before.equals(model.focused() + "/" + model.overlay() + "/" + model.overlayFocus());
    }

    /** On every step of a state: the page offers A exactly when A does something there, and B likewise. */
    private static void promptsAreTrue(String state, Make make) {
        for (int step = 0; step < SetupModel.STEPS; ++step) {
            SetupModel shown = make.model();
            boolean question = shown.overlay() != SetupModel.OVERLAY_NONE;
            if (question && step > 0) return;   // under a question no step can be chosen: one look is all there is
            if (!question) shown.focus(step);
            List<MenuScenes.Prompt> prompts = ShellPages.setup(shown).prompts;
            for (int button : new int[] {A, B}) {
                SetupModel pressed = make.model();
                if (!question) pressed.focus(step);
                String where = state + (question ? "" : ", step " + (step + 1)) + ": " + (button == A ? "A" : "B");
                same(acts(pressed, button), prompt(prompts, button) != null, where + " is offered exactly when it does something");
            }
        }
    }

    public static void main(String[] args) throws Exception {
        begin("NavContractTest");

        test("P-1 setup page: on every step of every state A and B are offered exactly when they do something", () -> {
            promptsAreTrue("before the device was looked at", () -> new SetupModel());
            promptsAreTrue("nothing installed", () -> fresh());
            promptsAreTrue("an existing install (read-only)", () -> {
                SetupModel model = new SetupModel();
                model.setDisk(disk(new InstallGuard.Existing(true, false, false, false), 0, 10, false));
                return model;
            });
            promptsAreTrue("the chosen disc is being read", () -> {
                SetupModel model = fresh();
                model.identifyStarted();
                return model;
            });
            promptsAreTrue("the disc was recognised", () -> recognised());
            promptsAreTrue("the disc was not recognised", () -> {
                SetupModel model = fresh();
                model.identifyStarted();
                model.playDiscIdentified(new DiscIdentifier.Result(DiscIdentifier.Kind.UNREADABLE, null, "Unreadable.", "Choose it again."));
                return model;
            });
            promptsAreTrue("copying", () -> {
                SetupModel model = recognised();
                model.workStarted(SetupModel.STEP_COPY, 1000);
                return model;
            });
            promptsAreTrue("the files are copied, the game code is missing", () -> copied(false));
            promptsAreTrue("importing the game code", () -> {
                SetupModel model = copied(false);
                model.workStarted(SetupModel.STEP_IMPORT, 1000);
                return model;
            });
            promptsAreTrue("everything is in place", () -> copied(true));
            promptsAreTrue("the question where the disc is", () -> {
                SetupModel model = fresh();
                model.nav(A, false);
                return model;
            });
            promptsAreTrue("the question whether to stop copying", () -> {
                SetupModel model = recognised();
                model.workStarted(SetupModel.STEP_COPY, 1000);
                model.nav(B, false);
                return model;
            });
        });

        final InstallState ready = install();

        test("P-1 Play tab: a question that arrives while the highlight is on the tab strip is shown with what B does; a button that does nothing there leaves it open", () -> {
            // A on Play, then UP while the game code is still being checked: the highlight is on the strip when the
            // check ends with "not the qualified build".
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            ShellNav nav = new ShellNav();
            same(LauncherMenu.Action.PLAY, menu.tap(LauncherMenu.PRIMARY, false), "(A on Play)");
            nav.command(UP, false, false, false);
            ShellPages.afterMove(ShellNav.PLAY, nav, menu);
            menu.prepared(LaunchRequest.UNQUALIFIED, false, true);
            check(menu.asking(), "(the question is open, the highlight on the strip)");
            same(ShellNav.STRIP, nav.level(), "(the strip)");

            List<MenuScenes.Prompt> prompts = page(nav, menu).prompts;
            check(prompt(prompts, B) != null, "B answers the question from the strip too, so its prompt stands there");
            same(ShellPages.PROMPT_LEAVE_IT, prompt(prompts, B).label, "and says what B does");
            same("Select", prompt(prompts, A).label, "A goes into the page");
            same(ShellNav.TO_PLAY_QUESTION, ShellNav.route(nav.tab(), B, false, false, menu.asking()), "(B is the question's)");

            for (int nothing : new int[] {UP, START, Y}) {
                same(ShellNav.NOTHING, nav.command(nothing, false, false, false), "command " + nothing + " does nothing on the strip");
                ShellPages.afterMove(ShellNav.PLAY, nav, menu);
                check(menu.asking(), "command " + nothing + " has no prompt and answers nothing: the question is still open");
            }
            same(ShellNav.MOVED, nav.command(A, false, false, false), "A goes onto the Play item");
            ShellPages.afterMove(ShellNav.PLAY, nav, menu);
            check(menu.asking(), "with the question still open");
            same("Start Anyway", prompt(page(nav, menu).prompts, A).label, "where A now starts it anyway");

            nav.command(UP, false, false, false);
            ShellPages.afterMove(ShellNav.PLAY, nav, menu);
            check(!menu.asking(), "UP from the item takes the highlight away from the question: answered no");
            check(prompt(page(nav, menu).prompts, B) == null, "and without a question B does nothing on the strip: no prompt");

            // Along the strip to another tab: answered no as well.
            LauncherMenu other = new LauncherMenu(true);
            other.setInstall(ready);
            ShellNav strip = new ShellNav();
            other.tap(LauncherMenu.PRIMARY, false);
            strip.command(UP, false, false, false);
            ShellPages.afterMove(ShellNav.PLAY, strip, other);
            other.prepared(LaunchRequest.UNQUALIFIED, false, true);
            same(ShellNav.TAB, strip.command(LEFT, false, false, false), "LEFT on the strip shows another tab");
            ShellPages.afterMove(ShellNav.PLAY, strip, other);
            check(!other.asking(), "the question is answered no");
        });

        test("P-1 Controller tab: Select is not offered on the check while a start of the game is being prepared, where A does nothing", () -> {
            LauncherMenu menu = new LauncherMenu(true);
            menu.setInstall(ready);
            ShellNav nav = new ShellNav();
            nav.command(NavInput.PAGE_NEXT, false, false, false);
            nav.command(NavInput.PAGE_NEXT, false, false, false);
            same(ShellNav.CONTROLLER, nav.tab(), "(the Controller tab, on its item)");
            check(prompt(page(nav, menu).prompts, A) != null, "A opens the controller check");

            LauncherMenu preparing = new LauncherMenu(true);
            preparing.setInstall(ready);
            same(LauncherMenu.Action.PLAY, preparing.tap(LauncherMenu.PRIMARY, false), "(a start is being prepared)");
            List<MenuScenes.Prompt> prompts = page(nav, preparing).prompts;
            check(prompt(prompts, A) == null, "A is not offered");
            same(LauncherMenu.Action.NONE, preparing.tap(LauncherMenu.CONTROLLER_CHECK, false), "and A indeed does nothing");
            same("Back", prompt(prompts, B).label, "B still goes to the strip");
            nav.command(UP, false, false, false);
            check(prompt(page(nav, preparing).prompts, A) != null, "on the strip A goes into the page, whatever is being prepared");
            nav.command(NavInput.DOWN, false, false, false);
            check(!preparing.prepared(null, true, true), "(the preparation ends: the game is already running)");
            check(prompt(page(nav, preparing).prompts, A) != null, "then A is offered again");
        });

        test("P-1 Settings tab: while a save is being written no button does anything, so no prompt stands; they are back when it is written", () -> {
            SettingsPagesTest.Tab tab = new SettingsPagesTest.Tab(SettingsCoreTestKit.v464(), Settings.defaults(), true, new java.util.ArrayList<String>());
            tab.into(SettingsPages.DISPLAY, 0);
            tab.press(NavInput.RIGHT);
            int category = tab.nav.column(ShellNav.SETTINGS);
            List<String> before = labels(SettingsPages.prompts(tab.model, ShellNav.PANE, category));
            same(4, before.size(), "(on a changed row four buttons are offered: Save, Restore, Back, Select)");
            tab.press(START);
            tab.model.beginSave();

            String held = settingsState(tab.model);
            for (int command = NavInput.UP; command <= NavInput.ALT; ++command) {
                check(tab.model.nav(command, false), "command " + command + " is taken by the model while it saves");
                same(held, settingsState(tab.model), "and command " + command + " changes nothing");
            }
            for (int level : new int[] {ShellNav.STRIP, ShellNav.COLUMN, ShellNav.PANE}) {
                same(new java.util.ArrayList<String>(), labels(SettingsPages.prompts(tab.model, level, category)),
                        "so no prompt stands at level " + level + " while the save is written");
            }

            tab.model.saveFinished(true, false, null);
            same(before, labels(SettingsPages.prompts(tab.model, ShellNav.PANE, category)), "when it is written the prompts are back");
        });

        end();
    }

    private static List<String> labels(List<MenuScenes.Prompt> prompts) {
        List<String> labels = new java.util.ArrayList<String>();
        for (MenuScenes.Prompt prompt : prompts) labels.add(prompt.glyph + " " + prompt.label);
        return labels;
    }

    /** What a command could change in the settings model: highlight, values, question, pending action, status. */
    private static String settingsState(MenuModel model) {
        StringBuilder text = new StringBuilder();
        text.append(model.focused()).append('/').append(model.overlay()).append('/').append(model.pendingAction()).append('/')
                .append(model.dirty()).append('/').append(model.status());
        for (int row = 0; row < model.rowCount(); ++row) text.append('/').append(model.rowValue(row));
        return text.toString();
    }

    /** The page of the shown tab as LauncherScreen builds it (Play and Controller: the two that are ShellPages' alone). */
    private static MenuScenes.Page page(ShellNav nav, LauncherMenu menu) {
        MenuScenes.Page page = ShellPages.frame(nav, menu, "Pad", UTC);
        if (nav.tab() == ShellNav.PLAY) ShellPages.play(page, nav, menu, "Pad", UTC);
        else ShellPages.controller(page, nav, menu, "Pad");
        return page;
    }

    /** A real detection of a small directory tree in which the game data and the game code are present: READY. */
    private static InstallState install() throws Exception {
        File files = LauncherTestSupport.newFilesDir();
        LauncherTestSupport.addRun(files, "0123456789abcdef0123456789abcdef", 1_700_000_000_000L, LauncherTestSupport.addInput(files, "one.bin"));
        LauncherTestSupport.addCode(files, 4096);
        InstallState state = InstallState.detect(files);
        LauncherTestSupport.discard(files);
        if (state.problem != InstallState.Problem.NONE) throw new AssertionError("fixture: expected nothing in the way of Play, got " + state.problem);
        return state;
    }
}
