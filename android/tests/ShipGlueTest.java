package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

/**
 * The lines that connect the second repair's rules to Android. The rules themselves are pure Java and have their
 * own tests (SetupDiscModel.appLeft: SetupDiscModelTest; ShipVerify: ShipVerifyTest; LauncherMenu.takeWholeLook:
 * ShipLauncherReadyTest; ScreenStack.awaiting / restoresSetupPage: ScreenStackTest; ShipWords.selfTest:
 * ShipWordsTest). What calls them are screens and an activity, which no host test can run; this class holds, as
 * text, the one or two lines each that make the call, so that a rule cannot be left unconnected without a test
 * failing. Whitespace is ignored. Run with java -ea -Dapp.main=<the app's main source folder> (the runner sets it).
 *
 * What this does NOT show: that Android calls onStop, onResume, onSaveInstanceState and onActivityResult when and
 * in the order the comments say. That is Android's documented behaviour and is for the device steps.
 */
public final class ShipGlueTest {
    private static String source(String name) throws Exception {
        String main = System.getProperty("app.main");
        if (main == null && System.getProperty("repo.root") != null) {
            main = System.getProperty("repo.root") + "/android/app/src/main/java/dev/recompbootstrap/smoke";
        }
        expect(main != null, "-Dapp.main or -Drepo.root says where the app's sources are");
        return new String(Files.readAllBytes(new File(main, name).toPath()), StandardCharsets.UTF_8).replaceAll("\\s+", "");
    }

    private static void has(String source, String line, String what) {
        expect(source.contains(line.replaceAll("\\s+", "")), what + " (looked for: " + line + ")");
    }

    public static void main(String[] args) {
        check("leaving the app reaches the visible screen, and the two-disc page then stops its copy", () -> {
            String activity = source("BootstrapActivity.java");
            has(activity, "protected void onStop() { if (launcher != null) { launcher.stopped(); } super.onStop(); }", "the activity's onStop tells the host");
            has(activity, "void stopped() { Screen top = stack.top(); if (top != null && !destroyed) { top.onAppLeft(); } }", "the host tells the visible screen");
            has(source("SetupScreen.java"), "public void onAppLeft() { if (twoDisc != null) twoDisc.onAppLeft(); }", "the setup screen hands it to the two-disc page");
            String page = source("SetupDiscScreen.java");
            // setup-fixes: the model also tells the copy's marker, before the flag is set
            has(page, "@Override public void onAppLeft() { int disc = model.copyingRow(); if (model.appLeft(marker)) { cancel = true;",
                    "the two-disc page asks the model and sets the flag the copy reads at every block");
            has(page, "@Override public boolean cancelled() { return cancel; }", "which is the flag the copy's progress answers with");
        });

        check("coming back to the app reaches the visible screen: the menu looks again, the two-disc page looks at files and free space", () -> {
            has(source("BootstrapActivity.java"), "Screen top = stack.top(); if (resumedBefore && top != null) { top.onAppBack(); } resumedBefore = true;",
                    "every resume but the first tells the visible screen");
            has(source("LauncherScreen.java"), "@Override public void onAppBack() { refresh(); }", "the menu looks at the installed files again");
            has(source("SetupScreen.java"), "public void onAppBack() { if (twoDisc != null) twoDisc.onAppBack(); }", "the setup screen hands it on");
            // setup-fixes: through the look gate (no second look while one runs; none while reading or copying)
            has(source("SetupDiscScreen.java"), "@Override public void onAppBack() { SetupDiscModel.Look look = shown ? model.lookStarts() : null;"
                    + " if (look != null) inspect(false, look); }", "the two-disc page looks again unless it is reading or copying or looking");
        });

        check("an activity made anew under the file chooser shows the setup page again and gives it the answer", () -> {
            String activity = source("BootstrapActivity.java");
            has(activity, "protected void onSaveInstanceState(Bundle outState) { super.onSaveInstanceState(outState); if (launcher != null) {"
                    + " String awaiting = launcher.awaiting(); if (awaiting != null) { outState.putString(ScreenStack.AWAITING_KEY, awaiting); } } }",
                    "what is saved is ScreenStack.awaiting's answer");
            has(activity, "String awaiting() { Screen target = resultTarget;"
                    + " return ScreenStack.awaiting(target != null && stack.contains(target), target instanceof SetupScreen); }",
                    "asked for the screen that waits for the answer");
            has(activity, "launcher.create(savedInstanceState == null ? null : savedInstanceState.getString(ScreenStack.AWAITING_KEY));",
                    "the new activity reads what was saved");
            int restored = activity.indexOf("if(ScreenStack.restoresSetupPage(savedAwaiting)){");
            int menu = activity.indexOf("stack.push(newLauncherScreen());");
            expect(menu > 0 && restored > menu, "the setup page is restored on top of the menu");
            String block = activity.substring(restored, activity.indexOf("}elseif(ScreenStack.setupRequested(", restored));
            has(block, "Screen setup = new SetupScreen(); stack.push(setup); resultTarget = setup;", "it is pushed and made the one the answer goes to");
            has(activity, "Screen target = resultTarget; resultTarget = null; if (target != null && stack.contains(target)) {"
                    + " target.onActivityResult(requestCode, resultCode, data); }", "and the answer is delivered to it as to any waiting screen");
        });

        check("the menu's three pages ask for \"ready\" through the remembered read, and the read's lines go to the log", () -> {
            String launcher = source("LauncherScreen.java"), about = source("AboutScreen.java"), page = source("SetupDiscScreen.java");
            has(launcher, "final boolean wholeLook = menu.takeWholeLook(); host.background(new Runnable() { @Override public void run() {",
                    "the Play tab's look asks the menu, on the UI thread, whether this look follows a return");
            has(launcher, "if (wholeLook) ShipVerify.again(); ship[0] = ShipReady.remembered(apkPath, filesDir, ShipVerify.WHY_MENU);",
                    "and then reads anew or answers from the last read");
            has(about, "shipReady[0] = ShipReady.remembered(apkPath, filesDir, ShipVerify.WHY_ABOUT);", "the About tab");
            has(page, "ShipReady.remembered(apk, filesDir, ShipVerify.WHY_SETUP_PAGE)", "the two-disc page");
            for (String screen : new String[] {launcher, about, page}) {
                expect(!screen.contains("ShipReady.of("), "none of the three reads the two files on its own account");
                expect(screen.indexOf("ShipVerify.again()") < 0 || screen == launcher, "only the menu's own look asks for a new read");
            }
            has(source("ShipApplication.java"), "ShipVerify.setSink(new ShipVerify.Sink() { @Override public void line(String line) {"
                    + " android.util.Log.i(ShipLog.TAG, line); } });", "the lines go to Android's log under the tag RecompShip");
        });

        check("the graphics test screen of a build that carries its game code draws the plain text", () -> {
            // menu-input-fixes D1: the text comes from SelfTestRun, which applies ShipWords.selfTest to the report in the
            // handed-out build (SelfTestRunTest checks text(true) == ShipWords.selfTest(report)).
            has(source("BootstrapActivity.java"), "statusView.setText(selfTest.text(ShipApplication.carriesGameCode()));",
                    "the handed-out build shows ShipWords.selfTest's lines, the developer build the whole report");
            has(source("SelfTestRun.java"), "return ship ? ShipWords.selfTest(report) : report;", "SelfTestRun words the report so");
        });

        ShipTestKit.finish("ShipGlueTest");
    }
}
