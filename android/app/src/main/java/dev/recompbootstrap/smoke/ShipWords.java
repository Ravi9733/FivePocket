package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.List;

/**
 * The last look at a page before it is drawn in the build that is handed out: the menu's sentences were written for
 * the developer build on the owner's phone, and some of them name that phone, the kind of build, the install kit or a
 * version of the game code. A player of the handed-out build has none of these. Pure Java.
 *
 * LauncherScreen passes every tab's page through page() when the build carries its game code (LauncherMenu
 * .selfInstalling). The Play, Controller and About tabs already speak ShipAdvice's words there, so for them this
 * changes nothing; it is what makes the Settings tab's measured notes and reasons read right, and it is the one
 * place that guarantees the rule for every tab. ShipWordsTest reads every sentence of every tab through it.
 *
 * It only rewords. It never hides a row, a warning or a failure.
 */
final class ShipWords {
    private ShipWords() {}

    /** What a measured note says instead of naming the owner's phone. */
    static final String MEASURED = "Measured on one test device";
    static final String SAVE_FAILED = MenuModel.TEXT_SAVE_FAILED + "a settings file could not be written. Check that this device has free"
            + " storage, then save again.";
    static final String STANDING = "Some settings shown here are overruled by test files on this device.";
    /** In place of "run the install kit": what a player of this build can do, with the controller. */
    private static final String AGAIN = ShipAdvice.FINISH_FROM_ANY_TAB;

    /** {what the developer build says, what the handed-out build says}; applied in this order. */
    private static final String[][] REWORDED = {
            {" The Thor's second-screen dashboard makes some frames uneven.", ""},
            {"Measured on an AYN Thor", MEASURED},
            {"\nPrivate build - not for distribution.", ""},
            {"Private build - not for distribution.", ""},
            {" (needs game build v463 or newer)", ""},
            {" in game builds from v461", ""},
            {"Not in the installed game build", "Not in this version"},
            {" and an app build that has this option", ""},
            {"No game build can run without a frame cap yet", "Not in this version"},
            // After the two lines above: "Not available: Not in the installed game build" reads as one sentence.
            {SettingsPages.TEXT_NOT_AVAILABLE + "Not in this version", SettingsPages.TEXT_NOT_IN_VERSION},
            {" To restore them, run the install kit again.", ""},
            {" To put the recorded version back, run the install kit again.", ""},
            {" The install kit writes new ones.", ""},
            {" The install kit copies them to this device.", ""},
            {" The install kit copies it to this device.", ""},
            {" The install kit copies it here.", ""},
            {"The install kit puts the right file in place. ", ""},
            {LaunchAdvice.KIT_RUN, AGAIN},
            {LaunchAdvice.KIT_AGAIN, AGAIN},
            {"connect this device to the computer with a USB cable and run the install kit again.", ShipAdvice.INSTALL_OVER_LOWER},
            {LaunchAdvice.KIT_APP, ShipAdvice.INSTALL_OVER},
            {"install the app again with the install kit.", ShipAdvice.INSTALL_OVER_LOWER},
            {"this phone", "this device"},
            {"This phone", "This device"},
    };

    /** What the handed-out build's graphics test screen says when the test could not start at all. */
    static final String SELF_TEST_FAILED = "The graphics test could not start on this device.";
    private static final String[] SELF_TEST_LINES = {"Renderer: ", "Surface: ", "Vulkan: "};

    /**
     * The graphics test screen's text in the build that is handed out: of the native test's report only the lines
     * about the picture (the renderer's state, the surface's size, what Vulkan said). The developer build shows the
     * whole report; its other lines ("Native library loaded", "ABI", "Lifecycle", "Guest address space", "Game
     * executable: not loaded; no guest execution") are a developer's, and the last one read as untrue to a player
     * whose game is installed (flow check, 2026-10-06).
     */
    static String selfTest(String report) {
        StringBuilder out = new StringBuilder();
        if (report != null) {
            for (String line : report.split("\n")) {
                for (String kept : SELF_TEST_LINES) {
                    if (line.startsWith(kept)) out.append(out.length() == 0 ? "" : "\n").append(line);
                }
            }
        }
        return out.length() == 0 ? SELF_TEST_FAILED : out.toString();
    }

    /** One sentence or text as the handed-out build shows it. null stays null. */
    static String plain(String text) {
        if (text == null || text.isEmpty()) {
            return text;
        }
        if (text.startsWith(SettingsStore.WARNING_STANDING)) {
            return STANDING;   // the developer build lists the files' names; they say nothing to a player
        }
        if (text.startsWith(MenuModel.TEXT_SAVE_FAILED) && (text.contains("private-trace/") || text.contains("settings/") || text.contains("/data/"))) {
            return SAVE_FAILED;   // the developer build names the file that could not be written
        }
        String out = text;
        for (String[] pair : REWORDED) {
            if (out.contains(pair[0])) {
                out = out.replace(pair[0], pair[1]);
            }
        }
        return out;
    }

    private static List<MenuScenes.Line> plain(List<MenuScenes.Line> lines) {
        List<MenuScenes.Line> out = new ArrayList<MenuScenes.Line>();
        for (MenuScenes.Line line : lines) {
            String text = plain(line.text);
            // A line that was only a developer note is not drawn as an empty line.
            if (text != null && text.isEmpty() && line.text != null && !line.text.isEmpty()) continue;
            out.add(text == line.text ? line : new MenuScenes.Line(text, line.tone));
        }
        return out;
    }

    private static void replace(List<MenuScenes.Line> lines) {
        List<MenuScenes.Line> out = plain(lines);
        lines.clear();
        lines.addAll(out);
    }

    /** Rewords everything a page shows, in place: the header's lines, notices, rows, help, text, footer and question. */
    static void page(MenuScenes.Page page) {
        List<MenuScenes.Line> status = new ArrayList<MenuScenes.Line>();
        for (MenuScenes.Line line : page.status) {
            if (!ShellPages.TEXT_PRIVATE_BUILD.equals(line.text)) status.add(line);   // the header says nothing about the kind of build
        }
        page.status.clear();
        page.status.addAll(plain(status));
        replace(page.info);
        replace(page.help);
        for (MenuScenes.Row row : page.rows) {
            row.label = plain(row.label);
            row.value = plain(row.value);
        }
        if (page.text != null) {
            List<String> text = new ArrayList<String>();
            for (String line : page.text) text.add(plain(line));
            page.text = text;
        }
        if (page.footerStatus != null) {
            page.footerStatus = new MenuScenes.Line(plain(page.footerStatus.text), page.footerStatus.tone);
        }
        if (page.alert != null) {
            page.alert.title = plain(page.alert.title);
            page.alert.detail = plain(page.alert.detail);
        }
    }
}
