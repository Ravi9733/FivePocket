package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.util.Arrays;

/**
 * The About tab's "Community" block (main's wording of 2026-11-05): the website and source, the Discord invite and
 * where to report bugs, shown as text on Licence Notices in both builds, right after the app's own GPLv3 notice and
 * before the notices of other software, and kept whole by the handed-out build's rewording and the pane's wrapping.
 * Run with java -ea.
 */
public final class CommunityLinksTest {
    /** The block's lines, held here independently of the app's constant. */
    private static final String[] LINES = {"Community", "Website and source: https://github.com/marcm0de/FivePocket",
            "Discord: https://discord.gg/mZvqCsxgxT", "Report bugs on Discord (#bug-reports) or GitHub Issues."};

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("CommunityLinksTest");

        test("Community: after the app's notice, before other software's notices, in both builds and as drawn", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                String block = String.join("\n", LINES) + "\n";
                for (boolean ship : new boolean[] {false, true}) {
                    String form = ship ? "the handed-out build" : "the developer build";
                    String notices = ship ? AboutScreen.licenceNoticesPlain() : AboutScreen.licenceNotices();
                    int at = notices.indexOf(block);
                    check(at >= 0 && at == notices.lastIndexOf(block), form + ": the block is shown once, its lines together");
                    int app = notices.indexOf("FivePocket is free software");
                    check(app >= 0 && app < at, form + ": after the app's own notice");
                    int others = notices.indexOf("Licence notices (");
                    check(others > at, form + ": before the notices of other software");
                    String drawn = AboutScreen.sectionText(ShellPages.ABOUT_NOTICES, "v", "", "", ship, "");
                    if (ship) drawn = ShipWords.plain(drawn);
                    check(drawn.contains(block), form + ": the drawn section keeps the block word for word");
                    String wrapped = String.join(" ", MenuScenes.paneText(drawn, true, MenuLayoutTest.FIXED));
                    String words = String.join(" ", Arrays.asList(wrapped.trim().split("\\s+")));
                    for (String line : LINES) check(words.contains(line), form + ": the wrapped pane keeps <" + line + "> whole");
                }
                check(ShipWordsTest.forbiddenIn(String.join("\n", LINES)) == null, "the block passes the handed-out build's word rule");
            }
        });

        SettingsCoreTestKit.end();
    }
}
