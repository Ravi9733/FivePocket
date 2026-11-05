package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.Arrays;

/**
 * This app's own GPLv3 notice (section 5(d), "Appropriate Legal Notices"): the owner's wording of 2026-11-05, word for
 * word, in MoreNotices and on the About tab's Licence Notices in both builds, before the notices of other software; and
 * the GPL text packaged as an asset, byte for byte the repository's COPYING, which the public build lets through.
 * Needs -Drepo.root=<repository root>. Run with java -ea.
 */
public final class AppNoticeTest {
    /** The owner's wording, held here independently of the app's constant. */
    private static final String WORDING = "FivePocket is free software: you can redistribute it and/or modify it under the terms of the GNU "
            + "General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) "
            + "any later version. FivePocket is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the "
            + "implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details. "
            + "The complete corresponding source code is published with every release as the FivePocket source archive. FivePocket "
            + "contains no game code or game data: you build those yourself from your own discs. FivePocket is not affiliated with or "
            + "endorsed by Rockstar Games, Take-Two Interactive or Microsoft.";

    private static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("AppNoticeTest");
        final File root = new File(System.getProperty("repo.root", "."));

        test("the app's GPLv3 notice: word for word in MoreNotices and first on Licence Notices, in both builds and as drawn", new SettingsCoreTestKit.Case() {
            @Override public void run() {
                same(WORDING, MoreNotices.APP_NOTICE, "MoreNotices holds the owner's wording, word for word");
                for (boolean ship : new boolean[] {false, true}) {
                    String form = ship ? "the handed-out build" : "the developer build";
                    String notices = ship ? AboutScreen.licenceNoticesPlain() : AboutScreen.licenceNotices();
                    check(notices.startsWith(MoreNotices.APP + MoreNotices.RULE), form + ": the app's notice opens the notices");
                    int at = notices.indexOf(WORDING);
                    check(at >= 0 && at == notices.lastIndexOf(WORDING), form + ": shown once");
                    for (String other : new String[] {"AMD FidelityFX Super Resolution 1", FontNotice.TEXT, MoreNotices.XENIA, MoreNotices.LGPL21}) {
                        check(notices.indexOf(other) > at, form + ": before the notice of other software <" + other.substring(0, 20) + ">");
                    }
                    // what the About tab draws: the joined lines, and in the handed-out build after the rewording
                    String drawn = AboutScreen.sectionText(ShellPages.ABOUT_NOTICES, "v", "", "", ship, "");
                    if (ship) drawn = ShipWords.plain(drawn);
                    check(drawn.contains(WORDING), form + ": the drawn section keeps the wording, word for word");
                    // the pane wraps the text into lines: every word stays, in order
                    String wrapped = String.join(" ", MenuScenes.paneText(drawn, true, MenuLayoutTest.FIXED));
                    String words = String.join(" ", Arrays.asList(wrapped.trim().split("\\s+")));
                    check(words.contains(WORDING), form + ": the wrapped pane lines hold every word in order");
                }
                check(ShipWordsTest.forbiddenIn(MoreNotices.APP) == null, "the notice passes the handed-out build's word rule as the app's own words");
            }
        });

        test("the GPL text: packaged as assets/licenses/GPL-3.0.txt, byte for byte COPYING, and allowed into the public build", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                File copying = new File(root, "COPYING");
                File asset = new File(root, "android/app/src/main/assets/licenses/GPL-3.0.txt");
                check(read(copying).contains("GNU GENERAL PUBLIC LICENSE") && read(copying).contains("Version 3, 29 June 2007"), "(COPYING is the GPLv3 text)");
                check(Arrays.equals(Files.readAllBytes(copying.toPath()), Files.readAllBytes(asset.toPath())), "the asset is COPYING, byte for byte");
                String gradle = read(new File(root, "android/app/build.gradle.kts"));
                int allow = gradle.indexOf("if (shared != setOf(");
                check(allow > 0 && gradle.indexOf("\"licenses/GPL-3.0.txt\"", allow) > allow
                        && gradle.indexOf("\"licenses/GPL-3.0.txt\"", allow) < gradle.indexOf("An unreviewed shared asset", allow),
                        "prePublicBuild's shared-asset allowlist names it");
            }
        });

        SettingsCoreTestKit.end();
    }
}
