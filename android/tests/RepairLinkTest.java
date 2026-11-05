package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.ID_A;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addCode;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addInput;
import static dev.recompbootstrap.smoke.LauncherTestSupport.addRun;
import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.discard;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.newFilesDir;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;

import java.io.File;

/**
 * Host tests of connections between classes that were written by different lanes of the app front end, added by the
 * repair step after the review. Each test joins the real producer to the real consumer; the lanes' own tests cover
 * each half. Run with java -ea, the whole app tree on the class path.
 */
public final class RepairLinkTest {
    public static void main(String[] args) throws Exception {
        LauncherTestSupport.requireAssertions();

        run("RepairLink.aFailedSaveIsShownWithOnePrefix", () -> {
            // The settings folder cannot be created because a plain file stands in its place.
            File root = java.nio.file.Files.createTempDirectory("repair-link").toFile();
            File files = new File(root, "files");
            check(new File(files, "private-trace").mkdirs(), "test setup");
            check(new File(files, "settings").createNewFile(), "test setup: a file where the settings folder must be");
            Capabilities none = Capabilities.of(new java.util.HashSet<String>(), false, false);
            SettingsStore.SaveResult result = new SettingsStore(files, new SettingsStore.GameRunning() {
                @Override public boolean check() { return false; }
            }).save(Settings.defaults(), none);
            check(!result.ok && result.error != null, "the save fails");
            MenuModel model = new MenuModel();
            model.loaded(Settings.defaults(), false, new java.util.ArrayList<String>(), new java.util.ArrayList<String>(), none);
            model.nav(NavInput.MENU, false);
            model.beginSave();
            model.saveFinished(result.ok, result.refusedGameRunning, result.error);   // as SettingsScreen does
            String status = model.status();
            equal("Could not save: Could not create the settings folder", status, "the footer line");
            equal(status.indexOf("Could not save"), status.lastIndexOf("Could not save"), "the prefix appears once");
            new File(files, "settings").delete();
            new File(files, "private-trace").delete();
            files.delete();
            root.delete();
        });

        run("RepairLink.theNoteOfASaveDuringWhichTheGameAppearedReachesTheFooter", () -> {
            File root = java.nio.file.Files.createTempDirectory("repair-link").toFile();
            File files = new File(root, "files");
            check(new File(files, "private-trace").mkdirs(), "test setup");
            Capabilities none = Capabilities.of(new java.util.HashSet<String>(), false, false);
            final int[] asked = {0};
            SettingsStore.SaveResult result = new SettingsStore(files, new SettingsStore.GameRunning() {
                @Override public boolean check() { return ++asked[0] > 1; }
            }).save(Settings.defaults(), none);
            check(result.ok, "the save completed: " + result.error);
            MenuModel model = new MenuModel();
            model.loaded(Settings.defaults(), false, new java.util.ArrayList<String>(), new java.util.ArrayList<String>(), none);
            model.nav(NavInput.MENU, false);
            model.beginSave();
            model.saveFinished(result.ok, result.refusedGameRunning, result.error);
            equal(SettingsStore.NOTE_GAME_APPEARED, model.status(), "the footer line");
            discardTree(root);
        });

        run("RepairLink.theStoresDeveloperSwitchWarningIsTheOneTheScreenKeepsAfterASave", () -> {
            equal(SettingsStore.WARNING_STANDING, MenuModel.TEXT_STANDING, "the two classes use the same sentence start");
            equal(SettingsStore.ERROR_GAME_RUNNING, MenuModel.TEXT_GAME_RUNNING, "and the same game-running sentence");
        });

        run("RepairLink.everyKeyTheMenusUseReachesThemAndEveryOtherGamepadButtonIsSwallowed", () -> {
            for (int code = 0; code <= 320; ++code) {
                final int[] commands = {0};
                NavInput input = new NavInput(new NavInput.Sink() {
                    @Override public void onNav(int command, boolean repeat) { ++commands[0]; }
                }, new NavInput.Timer() {
                    @Override public long now() { return 0; }
                    @Override public void schedule(Runnable task, long delayMs) { }
                    @Override public void cancel() { }
                });
                boolean menuKey = input.key(code, true, 0);
                boolean swallowed = ScreenStack.ignoresKey(code, 0);
                check(!(menuKey && swallowed), "key " + code + " is a menu key and must not be swallowed before the menu sees it");
                boolean gamepadButton = (code >= 96 && code <= 110) || (code >= 188 && code <= 203);
                if (gamepadButton) {
                    check(menuKey || swallowed, "gamepad button " + code + " is neither used nor swallowed: Android would make up a key for it");
                } else {
                    check(!swallowed, "key " + code + " is not a gamepad button and must be left to Android");
                }
            }
        });

        run("RepairLink.aRunMadeByPlayIsOneTheGameActivityClosesWhenItComesBackUsed", () -> {
            File files = newFilesDir();
            addRun(files, ID_A, 1_700_000_000_000L, addInput(files, "one.bin"));
            addCode(files, 4096);
            LaunchRequest.Prepared first = LaunchRequest.prepare(files);
            equal(null, first.error, "Play 1");
            check(InputDispatchOption.closesUsedRequest(1, files, first.requestId), "the launcher's run, used: the activity closes itself");
            check(!InputDispatchOption.closesUsedRequest(1, files, ID_A), "the script's run it was copied from, used: refusal screen as before");
            check(!InputDispatchOption.closesUsedRequest(0, files, first.requestId), "the launcher's run, fresh: it runs");
            equal(LaunchRequest.RUNS_LIST, InputDispatchOption.LAUNCHER_RUNS, "both classes name the same list file");
            // The newest launcher run is always on the list, however many Plays came before.
            LaunchRequest.Prepared last = first;
            for (int play = 2; play <= 5; ++play) {
                last = LaunchRequest.prepare(files);
                equal(null, last.error, "Play " + play);
                check(InputDispatchOption.closesUsedRequest(1, files, last.requestId), "Play " + play + ": its run is on the list");
            }
            discard(files);
        });

        LauncherTestSupport.finish("RepairLinkTest");
    }

    private static void discardTree(File node) {
        File[] children = node.listFiles();
        if (children != null) for (File child : children) discardTree(child);
        node.delete();
    }
}
