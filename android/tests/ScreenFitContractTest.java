package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.SetupTestKit.check;
import static dev.recompbootstrap.smoke.SetupTestKit.expect;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * setup-fixes: what only Android runs, read from the sources (as LauncherContractTest does), so that the glue the
 * pure tests cannot reach is at least present and in the right place. A device must still show that it works
 * (DEVICE-STEPS.md of the lane): the insets, the font change, the system's list of read permissions.
 *   C3-1  the setup page reads the figures again when its window gets the focus back (the activity tells the top
 *         screen; SetupScreen hands it on; SetupDiscScreen inspects).
 *   C1-1  the copy's marker is written before SetupDiscCopy.run and removed after it, except after a stop for leaving.
 *   C4-4  a refused new choice gives its read permission back.
 *   C7-1  the self-test and Controller Check keep their text out of the system bars; Controller Check fits its text.
 *   8     a font change does not make the menu or Controller Check anew (fontScale in configChanges).
 * Arguments: the folder with the app's Java sources (main and debug together, or main; the debug folder is then
 * looked for beside it as in the tree) and the manifest. Without arguments: the tree under -Drepo.root.
 * Run with java -ea.
 */
public final class ScreenFitContractTest {
    static File main, debug, manifest;

    static String source(String name) throws Exception {
        File f = new File(main, name);
        if (!f.isFile()) f = new File(debug, name);
        return new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8);
    }

    /** The body of the first method whose declaration matches, up to its closing brace at the same depth. */
    static String body(String text, String declaration) {
        Matcher m = Pattern.compile(declaration).matcher(text);
        if (!m.find()) throw new AssertionError("no method " + declaration);
        int open = text.indexOf('{', m.end());
        int depth = 0;
        for (int i = open; i < text.length(); ++i) {
            if (text.charAt(i) == '{') ++depth;
            else if (text.charAt(i) == '}' && --depth == 0) return text.substring(open, i + 1);
        }
        throw new AssertionError("unbalanced " + declaration);
    }

    /** The attributes of one activity element of the manifest. */
    static String activity(String manifestText, String name) {
        Matcher m = Pattern.compile("<activity[^>]*android:name=\"\\." + name + "\"[^>]*>", Pattern.DOTALL).matcher(manifestText);
        if (!m.find()) throw new AssertionError("no activity " + name);
        return m.group();
    }

    public static void main(String[] args) throws Exception {
        SetupTestKit.requireAssertions();
        String root = System.getProperty("repo.root", ".");
        main = new File(args.length > 0 ? args[0] : root + "/android/app/src/main/java/dev/recompbootstrap/smoke");
        debug = new File(root + "/android/app/src/debug/java/dev/recompbootstrap/smoke");
        manifest = new File(args.length > 1 ? args[1] : root + "/android/app/src/main/AndroidManifest.xml");

        check("C3-1: the window's focus coming back reaches the setup page, which looks at the files and the free space again", () -> {
            String host = body(source("BootstrapActivity.java"), "void focusChanged\\(boolean focused\\)");
            expect(host.contains("focusRelay.changed(focused)") && host.contains(".onWindowBack()"), "BootstrapActivity.focusChanged asks"
                    + " FocusRelay and tells the top screen: " + host);
            expect(body(source("SetupScreen.java"), "public void onWindowBack\\(\\)").contains("twoDisc.onWindowBack()"), "SetupScreen hands it on");
            String page = body(source("SetupDiscScreen.java"), "public void onWindowBack\\(\\)");
            // Every look goes through the gate: inspect() takes the token only lookStarts() makes (its constructor is private),
            // so a look without the gate does not compile; each caller takes the token in its own body (check-2 minor 1).
            String screen = source("SetupDiscScreen.java");
            expect(screen.contains("private void inspect(final boolean logPage, final SetupDiscModel.Look look)"), "inspect takes the token");
            for (String caller : new String[] {"public void onShown\\(\\)", "public void onAppBack\\(\\)", "public void onWindowBack\\(\\)",
                    "private void perform\\(int action\\)", "private void forgetLater\\("}) {
                String b = body(screen, caller);
                expect(b.contains("model.lookStarts()") && b.contains("if (look != null) inspect("), caller + " looks through the gate: " + b);
            }
            expect(!screen.contains("new SetupDiscModel.Look"), "no token is made outside the model");
            String inspect = body(screen, "private void inspect\\(");
            expect(inspect.contains("model.lookDone(look)"), "the look releases its own token");
            expect(inspect.contains("if (look == null) return;"), "a look without a token does nothing");
            expect(body(screen, "private void forgetLater\\(").contains("model.lookLater()"), "forgetLater follows a running look");
            expect(body(source("SetupDiscScreen.java"), "public void onShown\\(\\)").contains("inspect("), "and onShown still does");
            expect(body(source("SetupDiscScreen.java"), "public void onAppBack\\(\\)").contains("inspect("), "and onAppBack still does");
        });

        check("C1-1: the screen makes one marker per copy, begins it before the copy and ends it after; leaving tells it", () -> {
            String start = body(source("SetupDiscScreen.java"), "private void startCopy\\(final int disc\\)");
            int made = start.indexOf(".marker(disc)"), begins = start.indexOf("running.begin()"), run = start.indexOf("SetupDiscCopy.run("),
                    ended = start.indexOf("running.end(outcome[0].stopped && outcome[0].error == null);");
            expect(made >= 0 && begins > made && run > begins, "a new marker, begun before SetupDiscCopy.run");
            expect(ended > run, "ended after it, with the outcome's stop");
            // check-2 minors 1 and 2: the marker is told by the model, inside its "a copy is to stop" rule, and before the
            // copy's cancel flag is set
            String left = body(source("SetupDiscScreen.java"), "public void onAppLeft\\(\\)");
            int told = left.indexOf("if (model.appLeft(marker))"), cancel = left.indexOf("cancel = true");
            expect(told >= 0 && cancel > told, "onAppLeft: model.appLeft(marker), then cancel: " + left);
            expect(!left.contains("marker.appLeft()"), "the screen does not tell the marker itself");
        });

        check("C4-4: a refused new choice and a chosen file that cannot be opened give their read permission back", () -> {
            String identify = body(source("SetupDiscScreen.java"), "private void identify\\(");
            Matcher refused = Pattern.compile("SetupDiscModel\\.releaseGrant\\(permission, r\\.disc\\)\\) \\{\\s*(//[^\\n]*\\s*)*release\\(resolver, uri\\);")
                    .matcher(identify);
            expect(refused.find(), "refused: releaseGrant, then release");
            Matcher unreadable = Pattern.compile("SetupDiscModel\\.releaseGrant\\(permission, -1\\)\\) \\{\\s*release\\(resolver, uri\\);").matcher(identify);
            expect(unreadable.find(), "unreadable: the same");
        });

        check("C7-1: the self-test's text and Controller Check's keep out of the system bars and the cutout; Controller Check fits its text", () -> {
            String boot = source("BootstrapActivity.java");
            String create = body(boot, "protected void onCreate\\(Bundle savedInstanceState\\)");
            expect(create.contains("content.setOnApplyWindowInsetsListener("), "the self-test's views take the insets");
            expect(create.contains("headingView.setPadding(") && create.contains("statusView.setPadding("), "title and status lines are padded");
            expect(boot.contains("WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout()"), "bars and cutout");
            String check = source("ControllerCheckActivity.java");
            expect(check.contains("setOnApplyWindowInsetsListener(") && check.contains("WindowFit.padded("), "Controller Check takes the insets");
            expect(check.contains("setAutoSizeTextTypeUniformWithConfiguration(WindowFit.CHECK_TEXT_MIN_SP,WindowFit.CHECK_TEXT_SP,"),
                    "and makes its text smaller only when it would not fit");
            expect(!check.contains("new ScrollView("), "no scroll view the pad cannot scroll");
        });

        check("8: a font change does not make the menu or Controller Check anew; the self-test takes the new size", () -> {
            String text = new String(Files.readAllBytes(manifest.toPath()), StandardCharsets.UTF_8);
            for (String name : new String[] {"BootstrapActivity", "ControllerCheckActivity"}) {
                String a = activity(text, name);
                Matcher m = Pattern.compile("android:configChanges=\"([^\"]*)\"").matcher(a);
                expect(m.find(), name + " has configChanges");
                java.util.List<String> changes = java.util.Arrays.asList(m.group(1).split("\\|"));
                expect(changes.contains("fontScale"), name + " handles fontScale itself: " + m.group(1));
                expect(changes.contains("keyboard") && changes.contains("keyboardHidden") && changes.contains("navigation"), name + " keeps the rest");
            }
            String boot = body(source("BootstrapActivity.java"), "public void onConfigurationChanged\\(");
            expect(boot.contains("setTextSize("), "the self-test's text views take the new font size");
            expect(source("ControllerCheckActivity.java").contains("public void onConfigurationChanged("), "Controller Check refits its text");
        });

        SetupTestKit.finish("ScreenFitContractTest");
    }
}
