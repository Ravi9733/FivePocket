package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.ShipTestKit.check;
import static dev.recompbootstrap.smoke.ShipTestKit.expect;
import static dev.recompbootstrap.smoke.ShipTestKit.expectContains;
import static dev.recompbootstrap.smoke.ShipTestKit.expectEquals;

/**
 * What the first screen ("Setting up") says and offers in each situation, driven with the real results of
 * ShipFirstRun. The screen asks nothing and offers no folder; after a failure it offers "Try again". Run with java -ea.
 */
public final class ShipSetupModelTest {
    public static void main(String[] args) {
        check("a failed first start shows its sentence and offers Try again; A tries again, B closes; it never goes on by itself", () -> {
            ShipTestKit.Version v = new ShipTestKit.Version(ShipTestKit.newFilesDir("model-fail")).build();
            ShipFirstRun.Result failed = ShipFirstRun.run(v.bundle, v.files, ShipTestKit.QUIET, ShipTestKit.spaceOf(1000));
            ShipSetupModel model = new ShipSetupModel();
            model.prepareStarted();
            model.prepareFailed(failed.error);
            expectEquals(ShipSetupModel.Phase.PREPARE_FAILED, model.phase(), "phase");
            expectEquals("Setting up did not finish", model.heading(), "heading");
            expectContains(model.text(), "Not enough free space for the game code", "the text is the failure's sentence");
            expect(model.textIsProblem(), "drawn as a problem");
            expectEquals("Try again", model.primary(), "the one button");
            expectEquals(ShipSetupModel.ACT_RETRY, model.primaryAction(), "A");
            expectEquals(ShipSetupModel.ACT_CLOSE, model.backAction(), "B and Android's Back");
            expectContains(model.text(), "B: close the app.", "the first start says what B does");
            ShipSetupModel fromMenu = new ShipSetupModel();
            fromMenu.opened(true);
            fromMenu.prepareFailed(failed.error);
            expectContains(fromMenu.text(), "B: back to the menu.", "opened from the menu it says where B goes");
            // trying again is the working state again, with no button
            model.prepareStarted();
            expect(model.busy() && model.primary() == null && !model.textIsProblem(), "after Try again the screen works");
        });

        check("while it works the screen is 'Setting up' with the amount and a bar, and no button: nothing is asked", () -> {
            ShipSetupModel model = new ShipSetupModel();
            expectEquals("Setting up", model.heading(), "the heading while it looks at what is installed");
            expect(model.primary() == null && model.primaryAction() == ShipSetupModel.ACT_NONE, "no button while it looks");
            model.prepareStarted();
            expectEquals("Setting up", model.heading(), "the heading while it works");
            model.prepareProgress(ShipFirstRun.Step.GAME_CODE, 120000000L, 339203240L);
            expectContains(model.text(), "120 MB of 339 MB", "the amount");
            expect(Math.abs(model.fraction() - 120000000f / 339203240f) < 0.001f, "the bar: " + model.fraction());
            for (ShipFirstRun.Step step : ShipFirstRun.Step.values()) {
                model.prepareProgress(step, 1000000, 2000000);
                expect(model.busy() && model.primary() == null && model.primaryAction() == ShipSetupModel.ACT_NONE,
                        "no button and nothing for A while " + step);
                expectEquals(ShipSetupModel.ACT_NONE, model.backAction(), "B does not cut the work short while " + step);
                String said = model.text().toLowerCase(java.util.Locale.ROOT);
                expect(!said.contains("folder") && !said.contains("choose") && !said.contains("?"), "nothing is asked while " + step + ": " + said);
            }
            model.prepareProgress(ShipFirstRun.Step.GAME_DATA, 20000000L, 31342259L);
            expectContains(model.text(), "the small game files this app carries", "the step that installs the bundled game data is named");
            expectContains(model.text(), "20 MB of 31 MB", "with its amount");
            model.prepareProgress(ShipFirstRun.Step.SWITCHES, 0, 0);
            expectEquals(-1f, model.fraction(), "no bar for a step that copies nothing");
        });

        check("a failure is said by its kind and its next step: no file of the app's storage and no folder of the device on the screen", () -> {
            // a piece inside the APK that is not the recorded one
            ShipTestKit.Version damaged = new ShipTestKit.Version(ShipTestKit.newFilesDir("model-damaged"));
            damaged.bundledPaths.addAll(java.util.Arrays.asList(ShipTestKit.GENERATED));
            damaged.build();
            damaged.bundle.assets.put("ship/pieces/modules/boot/store/dataflow.spv", ShipTestKit.bytes(8964, 5));
            String raw = ShipFirstRun.run(damaged.bundle, damaged.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).error;
            expectContains(raw, "modules/boot/store/dataflow.spv", "(the work's own sentence names the piece: the log keeps it)");
            ShipSetupModel model = new ShipSetupModel();
            model.prepareFailed(raw);
            expectContains(model.text(), "This app's file is damaged or incomplete. Install the app again over this one", "a damaged app file");
            expect(ShipWordsTest.forbiddenIn(model.text()) == null && !model.text().contains("modules/"), "no file name on the screen: " + model.text());
            // the app's storage is not where its game code was built for (another user of the device)
            ShipTestKit.Version elsewhere = new ShipTestKit.Version(ShipTestKit.newFilesDir("model-elsewhere"));
            elsewhere.filesRoot = "/data/user/0/dev.recompbootstrap.smoke/files";
            elsewhere.build();
            raw = ShipFirstRun.run(elsewhere.bundle, elsewhere.files, ShipTestKit.QUIET, ShipTestKit.PLENTY).error;
            expectContains(raw, "/data/user/0/", "(the work's own sentence names both folders)");
            model.prepareFailed(raw);
            expectContains(model.text(), "This app only works for the main user of this device.", "another user of the device");
            expect(!model.text().contains("/data/"), "no folder of the device on the screen: " + model.text());
            // anything else is a file that could not be written
            model.prepareFailed("Could not put the game code in place: /data/user/0/x/files/private-trace/a.ship-part: No space left on device.");
            expectContains(model.text(), "The app could not put its files in place. Check that this device has free storage, then try again.", "a write failure");
            expect(ShipWordsTest.forbiddenIn(model.text()) == null, "plain: " + model.text());
            // the two that name only amounts or the next step are said as they are
            model.prepareFailed(ShipStart.GAME_RUNNING);
            expectContains(model.text(), "The game is running. Close the game, then try again", "the game is running");
        });

        check("finished: nothing is left to show or to press, whether or not game data is missing", () -> {
            ShipSetupModel model = new ShipSetupModel();
            model.prepareStarted();
            model.finished();
            expectEquals(ShipSetupModel.Phase.FINISHED, model.phase(), "phase");
            expect(model.primary() == null && model.backAction() == ShipSetupModel.ACT_NONE && !model.busy(), "no button, not working");
            expectEquals("", model.text(), "no text");
        });

        ShipTestKit.finish("ShipSetupModelTest");
    }
}
