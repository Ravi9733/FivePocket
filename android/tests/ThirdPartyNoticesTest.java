package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SettingsCoreTestKit.check;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.same;
import static dev.recompbootstrap.smoke.SettingsCoreTestKit.test;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The third-party notices of the game library: the tracked licence files exist with the full texts, and the About
 * screen shows both, word for word. Needs -Drepo.root=<repository root>. Run with java -ea.
 */
public final class ThirdPartyNoticesTest {
    private static String read(File file) throws IOException {
        return new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
    }

    public static void main(String[] args) {
        SettingsCoreTestKit.begin("ThirdPartyNoticesTest");
        final File root = new File(System.getProperty("repo.root", "."));

        test("Qualcomm Snapdragon GSR 1 (BSD-3-Clause): tracked licence file, and the whole text on the About screen", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                String licence = read(new File(root, "third_party/snapdragon-gsr/LICENSE"));
                check(licence.startsWith("Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved."), "the copyright line");
                for (String part : new String[] {"1. Redistributions of source code must retain", "2. Redistributions in binary form must reproduce",
                        "3. Neither the name of the copyright holder", "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"",
                        "SPDX-License-Identifier: BSD-3-Clause"}) {
                    check(licence.contains(part), "the licence file holds: " + part);
                }
                File upstream = new File(root, "local-analysis/thirdparty/snapdragon-gsr/LICENSE");
                if (upstream.isFile()) same(read(upstream), licence, "the tracked file is the downloaded repository's LICENSE, byte for byte");
                String trimmed = licence.endsWith("\n") ? licence : licence + "\n";
                check(AboutScreen.GSR1_NOTICE.endsWith(trimmed), "the About notice ends with the licence text, word for word");
                check(AboutScreen.GSR1_NOTICE.startsWith("Qualcomm Snapdragon Game Super Resolution 1"), "the notice names the software");
                check(AboutScreen.licenceNotices().contains(AboutScreen.GSR1_NOTICE), "the About screen shows the notice");
                // BSD clause 2: the copyright notice of what is compiled in. The shader sources carry a 2025 line, the
                // repository's LICENSE a 2023 line; the notice shows both.
                String carried = "Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved.";
                check(AboutScreen.GSR1_NOTICE.contains("The shader sources carry: " + carried + " SPDX-License-Identifier: BSD-3-Clause\n"),
                        "the notice shows the copyright line of the compiled shader sources");
                File shader = new File(root, "local-analysis/attempts/v461/src/direct_present_gsr.frag");
                if (shader.isFile()) check(read(shader).contains(carried), "that line is the one in direct_present_gsr.frag");
            }
        });

        test("AMD FidelityFX Super Resolution 1 (MIT): tracked licence file, and the whole text on the About screen", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                String licence = read(new File(root, "third_party/amd-fsr1/LICENSE"));
                String vendored = read(new File(root, "third_party/fsr1/license.txt"));
                check(licence.startsWith(vendored), "the file starts with AMD's license.txt as vendored beside the headers");
                check(licence.contains("Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved."), "AMD's copyright line");
                // the second notice the header ffx_a.h carries, line by line (comment marks and rule lines removed)
                int at = 0, lines = 0;
                boolean inside = false;
                for (String line : read(new File(root, "third_party/fsr1/ffx_a.h")).split("\n")) {
                    String text = line.startsWith("//") ? line.substring(2).trim() : null;
                    if (text != null && text.equals("MIT LICENSE")) inside = true;
                    if (!inside) continue;
                    if (text == null || text.startsWith("ABOUT") || text.startsWith("//")) break;
                    if (text.isEmpty() || text.matches("[=\\-]+")) continue;
                    int found = licence.indexOf(text, at);
                    check(found >= 0, "ffx_a.h's notice line is in the licence file, in order: " + text);
                    at = found + text.length();
                    lines++;
                }
                check(lines >= 10, "the notice of ffx_a.h was found in the header (" + lines + " lines)");
                check(AboutScreen.FSR1_NOTICE.endsWith(licence), "the About notice ends with the licence text, word for word");
                check(AboutScreen.licenceNotices().contains(AboutScreen.FSR1_NOTICE), "the About screen shows the notice");
                check(AboutScreen.licenceNotices().indexOf(AboutScreen.FSR1_NOTICE) < AboutScreen.licenceNotices().indexOf(AboutScreen.GSR1_NOTICE),
                        "both notices are shown, one after the other");
            }
        });

        test("the further notices (Xenia, ReXGlue, FFmpeg, XenonRecomp, SIMDe, the LGPL text): word for word as the notices file holds them, and all on the About screen", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                String file = read(new File(root, "docs/alpha/LICENCES.md"));
                check(file.contains("5. FFmpeg: libavcodec and libavutil") && file.contains("3. Xenia") && file.length() > 40000, "(the notices file was read)");
                String[][] texts = {{"Xenia", MoreNotices.XENIA, "Copyright (c) 2015, Ben Vanik. All rights reserved."},
                        {"ReXGlue", MoreNotices.REXGLUE, "Copyright (c) 2026, Tom Clay"},
                        {"XenonRecomp", MoreNotices.XENONRECOMP, "Copyright (c) 2025 hedge-dev and contributors"},
                        {"SIMDe", MoreNotices.SIMDE, "Copyright (c) 2017 Evan Nemerson"},
                        {"the LGPL", MoreNotices.LGPL21, "GNU LESSER GENERAL PUBLIC LICENSE"}};
                for (boolean ship : new boolean[] {false, true}) {
                    String shown = ship ? AboutScreen.licenceNoticesPlain() : AboutScreen.licenceNotices();
                    int last = -1;
                    for (String[] text : texts) {
                        check(text[1].length() > 1000 && text[1].contains(text[2]), text[0] + ": the text carries its copyright or title line");
                        check(file.contains(text[1]), text[0] + ": the text is in the notices file, word for word");
                        int at = shown.indexOf(text[1]);
                        check(at > last && shown.indexOf(text[1], at + 1) < 0, text[0] + ": shown once, in the order of the notices file" + (ship ? " (handed-out build)" : ""));
                        last = at;
                    }
                    for (String name : new String[] {"Xenia\n", "ReXGlue\n", "FFmpeg (libavcodec and libavutil)\n", "XenonRecomp\n", "SIMDe\n",
                            "Android NDK runtime libraries\n", "GNU Lesser General Public License, version 2.1\n",
                            "https://github.com/xenia-project/FFmpeg", "Revision: 15ece0882e8d5875051ff5b73c5a8326f7cee9f5\n",
                            "You may obtain the source code of the FFmpeg libraries"}) {
                        check(shown.contains(name), "the notices name <" + name.trim() + ">" + (ship ? " (handed-out build)" : ""));
                    }
                    check(shown.indexOf(AboutScreen.after3ForTest()) < shown.indexOf("Xenia\n"), "the further notices come after Qualcomm's");
                }
                // The LGPL text ends the notices and is whole: its first and its last line are there, 502 lines between them.
                check(MoreNotices.LGPL21.trim().endsWith("That's all there is to it!") && MoreNotices.LGPL21.split("\n", -1).length == 503, "the LGPL text has its 502 lines");
                check(file.endsWith(MoreNotices.LGPL21), "the notices file ends with the LGPL text");
                // (controls) a text that differs in one letter is not in the file: the comparison is exact.
                check(!file.contains(MoreNotices.XENIA.replace("Ben Vanik", "Ben Vanic")), "(control) one changed letter in Xenia's text is found");
                check(!file.contains(MoreNotices.LGPL21.replace("Lesser", "Lessor")), "(control) one changed letter in the LGPL text is found");
            }
        });

        test("Nunito Sans (SIL OFL 1.1), the menus' typeface: tracked licence file, the bundled font's hash, the whole text on the About screen", new SettingsCoreTestKit.Case() {
            @Override public void run() throws IOException {
                String licence = read(new File(root, "third_party/nunito-sans/OFL.txt"));
                check(licence.startsWith("Copyright 2016 The Nunito Sans Project Authors"), "the copyright line");
                check(licence.contains("SIL OPEN FONT LICENSE Version 1.1"), "the licence's name");
                same(licence, FontNotice.OFL, "the notice holds the licence file word for word");
                check(AboutScreen.licenceNotices().contains(FontNotice.TEXT), "the developer form of the notices shows it");
                check(AboutScreen.licenceNoticesPlain().contains(FontNotice.TEXT), "the plain form shows it");
                File font = new File(root, "android/app/src/main/assets/fonts/NunitoSans-VF.ttf");
                if (font.isFile()) {
                    String sum;
                    try {
                        java.security.MessageDigest sha = java.security.MessageDigest.getInstance("SHA-256");
                        StringBuilder hex = new StringBuilder();
                        for (byte b : sha.digest(Files.readAllBytes(font.toPath()))) hex.append(String.format("%02x", b));
                        sum = hex.toString();
                    } catch (java.security.NoSuchAlgorithmException none) {
                        throw new IOException(none);
                    }
                    check(read(new File(root, "third_party/nunito-sans/README.md")).contains(sum), "the bundled font is the recorded download (" + sum + ")");
                }
            }
        });

        SettingsCoreTestKit.end();
    }
}
