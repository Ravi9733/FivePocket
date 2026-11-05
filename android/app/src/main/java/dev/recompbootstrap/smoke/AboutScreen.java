package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import java.io.File;
import java.io.IOException;
import java.util.List;
import java.util.TimeZone;

/**
 * The About tab (local-analysis/agents/app-ui-4/DESIGN.md section 6): app version, what of the game is installed
 * (game code, game data, shader file, whether Play compares the game code's version), the settings that game code
 * lacks, and the licence notices, as sections at the left with the selected section's text at the right. The last
 * section, "Graphics self-test", opens the old Vulkan test screen. What is drawn is
 * ShellPages.about's; this class holds the texts and reads the installed files off the UI thread.
 */
final class AboutScreen {
    /**
     * The notice for AMD FSR 1, which the game's picture scaler uses. This is the text of
     * android/app/src/main/assets/licenses/AMD-FidelityFX-FSR1-MIT.txt, kept here because the app update changes no
     * packaged file other than the code (a host test compares the two).
     */
    static final String FSR1_NOTICE = ""
            + "AMD FidelityFX Super Resolution 1 (FSR 1): ffx_a.h, ffx_fsr1.h\n"
            + "Used by the presenter's FSR scaler (direct_present_fsr_easu.frag / _rcas.frag compiled into the library; FsrEasuCon / FsrRcasCon).\n"
            + "Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR (vendored unmodified in third_party/fsr1).\n"
            + "\n"
            + "Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.\n"
            + "\n"
            + "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
            + "of this software and associated documentation files (the \"Software\"), to deal\n"
            + "in the Software without restriction, including without limitation the rights\n"
            + "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
            + "copies of the Software, and to permit persons to whom the Software is\n"
            + "furnished to do so, subject to the following conditions:\n"
            + "\n"
            + "The above copyright notice and this permission notice shall be included in\n"
            + "all copies or substantial portions of the Software.\n"
            + "\n"
            + "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
            + "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
            + "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE\n"
            + "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
            + "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
            + "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN\n"
            + "THE SOFTWARE.\n"
            + "\n"
            + "ffx_a.h also carries the following notice (its float approximations, used by EASU and RCAS):\n"
            + "\n"
            + "MIT LICENSE\n"
            + "Copyright (c) 2014 Michal Drobot (for concepts used in \"FLOAT APPROXIMATIONS\").\n"
            + "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation\n"
            + "files (the \"Software\"), to deal in the Software without restriction, including without limitation the rights to use, copy,\n"
            + "modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the\n"
            + "Software is furnished to do so, subject to the following conditions:\n"
            + "The above copyright notice and this permission notice shall be included in all copies or substantial portions of the\n"
            + "Software.\n"
            + "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE\n"
            + "WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR\n"
            + "COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,\n"
            + "ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n";

    /**
     * The notice for Qualcomm Snapdragon Game Super Resolution 1 (BSD-3-Clause), which game builds from v461 contain.
     * After three heading lines this is the text of third_party/snapdragon-gsr/LICENSE, word for word (a host test
     * compares them). It is compiled into the app because an update of this kind replaces only the code of the
     * installed APK and cannot add an asset file.
     */
    static final String GSR1_NOTICE = ""
            + "Qualcomm Snapdragon Game Super Resolution 1 (GSR 1)\n"
            + "Used by the presenter's GSR scaler in game builds from v461 (direct_present_gsr.frag / _gsr_edge.frag, derived from Qualcomm's sgsr1_shader_mobile shaders and compiled into the library).\n"
            + "Source: https://github.com/SnapdragonStudios/snapdragon-gsr (licence text: third_party/snapdragon-gsr/LICENSE).\n"
            + "The shader sources carry: Copyright (c) 2025, Qualcomm Innovation Center, Inc. All rights reserved. SPDX-License-Identifier: BSD-3-Clause\n"
            + "\n"
            + "Copyright (c) 2023, Qualcomm Innovation Center, Inc. All rights reserved.\n"
            + "\n"
            + "Redistribution and use in source and binary forms, with or without\n"
            + "modification, are permitted provided that the following conditions are met:\n"
            + "\n"
            + "1. Redistributions of source code must retain the above copyright notice,\n"
            + "   this list of conditions and the following disclaimer.\n"
            + "\n"
            + "2. Redistributions in binary form must reproduce the above copyright notice,\n"
            + "   this list of conditions and the following disclaimer in the documentation\n"
            + "   and/or other materials provided with the distribution.\n"
            + "\n"
            + "3. Neither the name of the copyright holder nor the names of its contributors\n"
            + "   may be used to endorse or promote products derived from this software\n"
            + "   without specific prior written permission.\n"
            + "\n"
            + "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
            + "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
            + "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE\n"
            + "ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE\n"
            + "LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\n"
            + "CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\n"
            + "SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS\n"
            + "INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN\n"
            + "CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)\n"
            + "ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE\n"
            + "POSSIBILITY OF SUCH DAMAGE.\n"
            + "\n"
            + "SPDX-License-Identifier: BSD-3-Clause\n";

    private final ScreenHost host;
    private final LauncherScreen owner;
    private final ShellNav nav;
    private final String versionLine;
    private String codeLine = "Game code: checking\u2026";
    private String namesBlock = LaunchAdvice.ABOUT_CHECKING;
    /** The build that is handed out (it carries its game code): the sections use the player's words (ShipAdvice). */
    private final boolean ship = ShipApplication.playerApp();
    /** Captures: {pictures, bytes, not yet in the shared folder}; null while they are counted. */
    private long[] captures;
    private boolean copyingCaptures;
    private CaptureExport.Result captureResult;
    private List<String> lines;
    private int linesOf = -1, scrollOf, scroll;

    AboutScreen(ScreenHost host, LauncherScreen owner, ShellNav nav) {
        this.host = host;
        this.owner = owner;
        this.nav = nav;
        versionLine = versionLine(host.activity());
        nav.setColumnCount(ShellNav.ABOUT, ShellPages.ABOUT_SECTIONS.length);
    }

    @SuppressWarnings("deprecation")   // versionCode is the field that exists before API 28
    private static String versionLine(Activity context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            long code = Build.VERSION.SDK_INT >= 28 ? info.getLongVersionCode() : info.versionCode;
            return "App version " + info.versionName + " (version code " + code + ")";
        } catch (PackageManager.NameNotFoundException impossible) {
            return "App version unknown";
        }
    }

    /** The tab became the current one: read the installed game build again. */
    void selected() {
        final File filesDir = host.filesDir();
        final String apkPath = host.activity().getApplicationInfo().sourceDir;
        final boolean inputOption = inputOptionInApp();
        final InstallState[] install = new InstallState[1];
        final ShipReady[] shipReady = new ShipReady[1];
        final long[][] counted = new long[1][];
        final Capabilities[] capabilities = new Capabilities[1];
        final String[] problem = new String[1];
        final String[] publicCode = new String[1];
        final Activity activity = host.activity();
        captureResult = null;   // a new visit: the last copy's sentence has been read
        host.background(new Runnable() {
            @Override public void run() {
                // The same look as the Play tab's (LauncherScreen.detect), so that the two tabs count alike.
                shipReady[0] = ShipReady.remembered(apkPath, filesDir, ShipVerify.WHY_ABOUT);   // no second whole read for a tab
                install[0] = InstallState.detect(filesDir, shipReady[0]);
                if (ShipApplication.publicApp()) publicCode[0] = PublicGameCode.about(filesDir);
                counted[0] = countCaptures(activity, new File(filesDir, CaptureStore.FOLDER));
                try {
                    capabilities[0] = CapabilityScanner.load(filesDir, inputOption);
                } catch (IOException | RuntimeException failed) {
                    problem[0] = failed.getClass().getSimpleName();
                }
            }
        }, new Runnable() {
            @Override public void run() {
                codeLine = ship ? ShipAdvice.aboutBuild(install[0], shipReady[0]) : installBlock(install[0], TimeZone.getDefault());
                if (publicCode[0] != null) codeLine = publicCode[0];
                namesBlock = install[0].codeBytes > 0 ? namesBlock(capabilities[0], problem[0]) : "";
                captures = counted[0];
                linesOf = -1;
                owner.render();
            }
        });
    }

    /** The same test the settings tab uses for its input option, so both scan with the same answer. */
    private static boolean inputOptionInApp() {
        if (Build.VERSION.SDK_INT < 30) {
            return false;
        }
        try {
            Class.forName("dev.recompbootstrap.smoke.InputDispatchOption");
            return true;
        } catch (ClassNotFoundException | LinkageError absent) {
            return false;
        }
    }

    /** What of the game is installed: the game code line, then game data, shader file and the version check. */
    static String installBlock(InstallState install, TimeZone zone) {
        String code = install.codeBytes > 0
                ? "Game code: " + LauncherMenu.codeSizeAndDate(install.codeBytes, install.codeModifiedMs, zone)
                : "Game code: not installed";
        return code + "\n" + LaunchAdvice.aboutInstall(install);
    }

    /**
     * Which settings the installed game code cannot change, by the names their rows have on the Settings tab (a row
     * is unavailable there exactly when Capabilities.anyChoice is false for it). The low-delay input option is left
     * out: it is this app's and the Android version's, not the game code's. The names of the setting files the game
     * code reads, which stood here before, say nothing to a player.
     */
    static String namesBlock(Capabilities capabilities, String problem) {
        if (capabilities == null) {
            return "The game code file could not be read (" + problem + ").";
        }
        int settings = 0;
        List<String> missing = new java.util.ArrayList<String>();
        for (SettingSpec spec : SettingsSchema.all()) {
            if (spec.key.equals(SettingsSchema.INPUT_UNBUFFERED)) {
                continue;
            }
            ++settings;
            if (!capabilities.anyChoice(spec.key)) {
                missing.add(Scene.titleCase(spec.label));
            }
        }
        if (missing.isEmpty()) {
            return LaunchAdvice.ABOUT_NAMES_ALL;
        }
        if (missing.size() == settings) {
            return LaunchAdvice.ABOUT_NAMES_NONE;
        }
        StringBuilder block = new StringBuilder(LaunchAdvice.ABOUT_NAMES);
        for (String name : missing) {
            block.append("\n    ").append(name);
        }
        return block.toString();
    }

    /**
     * Everything the About tab shows under "Licence notices": AMD's and Qualcomm's full notices and the further ones
     * of MoreNotices (Xenia, ReXGlue, FFmpeg, XenonRecomp, SIMDe, the NDK runtime, the LGPL text), compiled into the code. (This
     * kind of app update replaces only the code of the installed APK, so the texts cannot be shipped as asset files.)
     * Before all of them stands this app's own GPLv3 notice (MoreNotices.APP), which is not a third-party text.
     */
    static String licenceNotices() {
        return MoreNotices.APP + MoreNotices.RULE + MoreNotices.COMMUNITY + MoreNotices.RULE
                + "Licence notices (third-party code inside the game library)\n\n" + FSR1_NOTICE
                + "\n----------------------------------------\n\n" + GSR1_NOTICE
                + "\n----------------------------------------\n\n" + FontNotice.TEXT
                + MoreNotices.RULE + MoreNotices.more();
    }

    /** The sentence of the tracked AMD licence file that joins its two notices; it names a source file. */
    private static final String FSR1_SECOND_NOTICE = "ffx_a.h also carries the following notice (its float approximations, used by EASU and RCAS):";
    static final String PLAIN_SECOND_NOTICE = "Part of it also carries the following notice:";
    static final String PLAIN_USE = "Used to scale the game's picture.";

    /** A notice without its first lines (the heading lines a developer reads). */
    private static String after(String notice, int lines) {
        int at = 0;
        for (int line = 0; line < lines; ++line) at = notice.indexOf('\n', at) + 1;
        return notice.substring(at);
    }

    /**
     * "Licence notices" as the build that is handed out shows them (the lead's decision of 2026-10-06): the two
     * product names, what each is used for, where it comes from, and every licence text in full, word for word as in
     * the developer build's text (the same characters: they are cut out of the two constants above, which host tests
     * hold against the licence files). What is left out are the developer's heading lines: the names of source
     * files and folders and the identifiers in them. This app's own GPLv3 notice (MoreNotices.APP) comes first, as in
     * the developer build's text.
     */
    static String licenceNoticesPlain() {
        return MoreNotices.APP + MoreNotices.RULE + MoreNotices.COMMUNITY + MoreNotices.RULE
                + "Licence notices (other software this app uses)\n\n"
                + "AMD FidelityFX Super Resolution 1 (FSR 1)\n" + PLAIN_USE + "\n"
                + "Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR\n"
                + after(FSR1_NOTICE, 3).replace(FSR1_SECOND_NOTICE, PLAIN_SECOND_NOTICE)
                + "\n----------------------------------------\n\n"
                + "Qualcomm Snapdragon Game Super Resolution 1 (GSR 1)\n" + PLAIN_USE + "\n"
                + "Source: https://github.com/SnapdragonStudios/snapdragon-gsr\n"
                + after(GSR1_NOTICE, 3)
                + "\n----------------------------------------\n\n" + FontNotice.TEXT
                + MoreNotices.RULE + MoreNotices.more();
    }

    /** Qualcomm's licence text without its heading lines: the part both forms of the notices share (for the host test). */
    static String after3ForTest() { return after(GSR1_NOTICE, 3); }

    /** The text of one section (ShellPages.ABOUT_*) in a developer build. */
    static String sectionText(int section, String versionLine, String codeLine, String namesBlock) {
        return sectionText(section, versionLine, codeLine, namesBlock, false, "");
    }

    /**
     * The text of one section. ship: the build that is handed out, which says nothing about the kind of build it is
     * (codeLine is then ShipAdvice.aboutBuild's). captures: CaptureAbout.text for the Captures section.
     */
    static String sectionText(int section, String versionLine, String codeLine, String namesBlock, boolean ship, String captures) {
        switch (section) {
            case ShellPages.ABOUT_APP: return versionLine + "\n" + (ship ? ShipAdvice.ABOUT_APP : "Private build - not for distribution.");
            case ShellPages.ABOUT_BUILD: return namesBlock.isEmpty() ? codeLine : codeLine + "\n\n" + namesBlock;
            case ShellPages.ABOUT_NOTICES: return Scene.reflow(ship ? licenceNoticesPlain() : licenceNotices());
            case ShellPages.ABOUT_CAPTURES: return captures;
            default: return ShellPages.TEXT_SELF_TEST;
        }
    }

    /**
     * A on the Captures section: copies the pictures that are not yet in the shared Pictures folder there, off the UI
     * thread, one copy at a time, and then says what was copied. With nothing to copy it does nothing.
     */
    void copyCaptures() {
        if (!CaptureAbout.canCopy(captures, copyingCaptures)) {
            return;
        }
        copyingCaptures = true;
        linesOf = -1;
        owner.render();
        final Activity activity = host.activity();
        final File root = new File(host.filesDir(), CaptureStore.FOLDER);
        final CaptureExport.Result[] result = new CaptureExport.Result[1];
        final long[][] counted = new long[1][];
        host.background(new Runnable() {
            @Override public void run() {
                try {
                    result[0] = CaptureMediaStore.copyToPictures(activity);
                } catch (RuntimeException refused) {   // a media store that misbehaves must not leave the section "Copying"
                    result[0] = new CaptureExport.Result(0, 0, CaptureStore.list(root).size(), refused.getClass().getSimpleName(), true);
                }
                android.util.Log.i("RecompNativeInput", "CAPTURE_EXPORT copied=" + result[0].copied + " already=" + result[0].already
                        + " total=" + result[0].total + " error=" + (result[0].error == null ? "none" : result[0].error));
                counted[0] = countCaptures(activity, root);
            }
        }, new Runnable() {
            @Override public void run() {
                copyingCaptures = false;
                captureResult = result[0];
                captures = counted[0];
                linesOf = -1;
                owner.render();
            }
        });
    }

    /**
     * The pictures, their size and how many are not in the shared Pictures folder (which is asked what it still
     * holds). A media store that misbehaves must not take the section away: the app's own list then counts.
     */
    private static long[] countCaptures(Activity activity, File root) {
        try {
            return CaptureMediaStore.summary(activity);
        } catch (RuntimeException refused) {
            return CaptureExport.summary(root);
        }
    }

    /** True when A on the Captures section would start a copy (the prompt is offered only then). */
    boolean capturesToCopy() {
        return CaptureAbout.canCopy(captures, copyingCaptures);
    }

    /** The selected section's text as lines of the pane's width; a newly selected section starts at its top. */
    private List<String> lines() {
        int section = nav.column(ShellNav.ABOUT);
        if (section != scrollOf) {
            scroll = 0;
            scrollOf = section;
        }
        if (lines == null || linesOf != section) {
            String text = sectionText(section, versionLine, codeLine, namesBlock, ship,
                    CaptureAbout.text(captures, copyingCaptures, captureResult));
            // Reworded before it is wrapped into lines: a phrase that ran over two lines could not be found afterwards.
            lines = MenuScenes.paneText(ship ? ShipWords.plain(text) : text, true, owner.measure());
            linesOf = section;
        }
        return lines;
    }

    /** True when the highlight can go into the text to scroll it. */
    boolean enterable() {
        int section = nav.column(ShellNav.ABOUT);
        return section != ShellPages.ABOUT_SELF_TEST && section != ShellPages.ABOUT_CAPTURES && ShellPages.aboutScrollable(lines().size());
    }

    /** A command while the highlight is in the text. */
    void paneCommand(int command) {
        if (command == NavInput.UP) scrollBy(-3);
        else if (command == NavInput.DOWN) scrollBy(3);
        else if (command == NavInput.LEFT) nav.place(ShellNav.ABOUT, nav.column(ShellNav.ABOUT), false);
    }

    /** Touch drag, or UP / DOWN: positive moves the text up. */
    void scrollBy(int count) {
        scroll = ShellPages.aboutScrolled(scroll, count, lines().size());
    }

    void fill(MenuScenes.Page page) {
        List<String> shown = lines();
        ShellPages.about(page, nav, shown, scroll, capturesToCopy());
    }

    /** Opens a second instance of the launcher activity in its old self-test mode; Back returns here. */
    void startSelfTest() {
        Activity activity = host.activity();
        Intent intent = new Intent(activity, BootstrapActivity.class);
        intent.putExtra("smoke", true);
        activity.startActivity(intent);
    }
}
