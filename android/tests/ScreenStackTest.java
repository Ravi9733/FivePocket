package dev.recompbootstrap.smoke;

import static dev.recompbootstrap.smoke.LauncherTestSupport.check;
import static dev.recompbootstrap.smoke.LauncherTestSupport.equal;
import static dev.recompbootstrap.smoke.LauncherTestSupport.run;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Host test of ScreenStack: which start intents select the old self-test, the panel scale, and the order in which
 * the host is told to attach, show, hide and detach screens. Run with java -ea. Screens are plain strings here.
 */
public final class ScreenStackTest {
    /** Records every call the stack makes, in order, e.g. "hidden launcher". */
    private static final class Recorder implements ScreenStack.Listener<String> {
        final List<String> calls = new ArrayList<String>();
        ScreenStack<String> stack;
        /** When a screen named here is shown, the given action runs from inside that call. */
        final Map<String, Runnable> whenShown = new HashMap<String, Runnable>();

        @Override public void attach(String screen) {
            calls.add("attach " + screen);
        }

        @Override public void detach(String screen) {
            calls.add("detach " + screen);
        }

        @Override public void shown(String screen) {
            calls.add("shown " + screen + " (top is " + stack.top() + ", size " + stack.size() + ")");
            Runnable action = whenShown.remove(screen);
            if (action != null) {
                action.run();
            }
        }

        @Override public void hidden(String screen) {
            calls.add("hidden " + screen);
        }

        List<String> take() {
            List<String> taken = new ArrayList<String>(calls);
            calls.clear();
            return taken;
        }
    }

    private static Recorder recorder() {
        Recorder recorder = new Recorder();
        recorder.stack = new ScreenStack<String>(recorder);
        return recorder;
    }

    private static ScreenStack.Extras extras(final Map<String, Object> values) {
        return new ScreenStack.Extras() {
            @Override public String text(String name) {
                Object value = values.get(name);
                return value instanceof String ? (String) value : null;
            }

            @Override public boolean flag(String name) {
                return Boolean.TRUE.equals(values.get(name));
            }
        };
    }

    private static Map<String, Object> map(Object... pairs) {
        Map<String, Object> values = new HashMap<String, Object>();
        for (int index = 0; index < pairs.length; index += 2) {
            values.put((String) pairs[index], pairs[index + 1]);
        }
        return values;
    }

    public static void main(String[] args) {
        LauncherTestSupport.requireAssertions();

        run("ScreenStack.onlyTheTwoKnownExtrasSelectTheOldSelfTest", () -> {
            // What "am start -n .../.BootstrapActivity" passes today, and what it must keep meaning.
            check(!ScreenStack.smokeRequested(extras(map())), "no extras: the launcher");
            for (String mode : new String[] {"pattern", "reject-readback", "stopped-preview", "unknown", ""}) {
                check(ScreenStack.smokeRequested(extras(map("rgba_diagnostic", mode))),
                        "--es rgba_diagnostic " + mode + ": the self-test");
            }
            check(ScreenStack.smokeRequested(extras(map("smoke", true))), "--ez smoke true: the self-test");
            check(!ScreenStack.smokeRequested(extras(map("smoke", false))), "--ez smoke false: the launcher");
            check(!ScreenStack.smokeRequested(extras(map("private_trace", "run-native-display", "request_id", "x"))),
                    "the game activity's extras mean nothing here");
            check(!ScreenStack.smokeRequested(extras(map("rgba-diagnostic", "pattern", "Smoke", true))),
                    "other spellings mean nothing");
        });

        run("ScreenStack.onlyADebuggableBuildOpensTheSetupPageAndOnlyForItsOneExtra", () -> {
            // "am start -n .../.BootstrapActivity --es open setup": the device check of the first-run setup page.
            check(ScreenStack.setupRequested(extras(map("open", "setup")), true), "--es open setup in a debuggable build: the setup page");
            check(!ScreenStack.setupRequested(extras(map("open", "setup")), false), "the same extra in a build that is not debuggable: the launcher alone");
            check(!ScreenStack.setupRequested(extras(map()), true), "no extras: the launcher alone");
            for (String other : new String[] {"settings", "Setup", "setup "}) {
                check(!ScreenStack.setupRequested(extras(map("open", other)), true), "--es open '" + other + "' opens nothing");
            }
        });

        run("ScreenStack.anActivityMadeAnewUnderTheFileChooserShowsTheSetupPageAgainForItsAnswer", () -> {
            // What the activity saves while the system's file chooser is open for the setup page ...
            String saved = ScreenStack.awaiting(true, true);
            check(saved != null, "the setup page waits for the chooser's answer: that is saved");
            // ... is what makes the activity Android made anew show the setup page again (the answer then goes to it).
            check(ScreenStack.restoresSetupPage(saved), "the new activity shows the setup page again");
            // Nothing is saved, and nothing restored, in every other case.
            equal(null, ScreenStack.awaiting(false, true), "the page that asked has been left since: nothing is saved");
            equal(null, ScreenStack.awaiting(true, false), "another screen waits (none does today): nothing is saved");
            equal(null, ScreenStack.awaiting(false, false), "no screen waits: nothing is saved");
            check(!ScreenStack.restoresSetupPage(null), "a first start, or an activity that waited for nothing: the menu alone");
            check(!ScreenStack.restoresSetupPage("launcher") && !ScreenStack.restoresSetupPage(""), "any other saved value: the menu alone");
        });

        run("ScreenStack.unitIsOneOnTheMainPanelAndFitsNarrowerWindows", () -> {
            equal(1f, ScreenStack.unit(1920, 1080), "the main panel");
            equal(1f, ScreenStack.unit(1080, 1920), "the sides in the other order");
            equal(2f, ScreenStack.unit(3840, 2160), "twice the pixels");
            equal(1f, ScreenStack.unit(2400, 1080), "a wider panel: the height decides");
            equal(1240 / 1920f, ScreenStack.unit(1240, 1080), "a narrower window: the width decides");
            equal(1f, ScreenStack.unit(0, 0), "no size known yet");
        });

        run("ScreenStack.pushAndPopTellTheHostInOrder", () -> {
            Recorder host = recorder();
            host.stack.push("launcher");
            equal(Arrays.asList("attach launcher", "shown launcher (top is launcher, size 1)"), host.take(), "first push");
            host.stack.push("settings");
            equal(Arrays.asList("hidden launcher", "attach settings", "shown settings (top is settings, size 2)"),
                    host.take(), "second push");
            host.stack.pop();
            equal(Arrays.asList("hidden settings", "detach settings", "shown launcher (top is launcher, size 1)"),
                    host.take(), "pop");
            equal("launcher", host.stack.top(), "top after the pop");
            check(!host.stack.contains("settings") && host.stack.contains("launcher"), "contains");
        });

        run("ScreenStack.theBottomScreenIsNeverPopped", () -> {
            Recorder host = recorder();
            host.stack.pop();
            equal(Arrays.asList(), host.take(), "pop on an empty stack");
            host.stack.push("launcher");
            host.take();
            host.stack.pop();
            host.stack.backNotHandledBy("launcher");
            equal(Arrays.asList(), host.take(), "pop and back at the bottom screen");
            equal(1, host.stack.size(), "the launcher is still there");
        });

        run("ScreenStack.unhandledBackPopsOnlyTheScreenThatWasAsked", () -> {
            Recorder host = recorder();
            host.stack.push("launcher");
            host.stack.push("settings");
            host.stack.push("overlay");
            host.take();
            host.stack.backNotHandledBy("overlay");
            equal("settings", host.stack.top(), "the asked screen was on top: popped");
            // "overlay" left by itself while it handled BACK and then still answered "not handled":
            host.stack.backNotHandledBy("overlay");
            equal("settings", host.stack.top(), "the screen below is not taken with it");
            host.stack.backNotHandledBy(null);
            equal(2, host.stack.size(), "nothing asked, nothing popped");
        });

        run("ScreenStack.aChangeAskedForDuringAChangeHappensAfterIt", () -> {
            // "setup" opens "picker" from inside its own onShown; the stack must be whole at every call.
            final Recorder host = recorder();
            host.stack.push("launcher");
            host.take();
            host.whenShown.put("setup", new Runnable() {
                @Override public void run() {
                    host.stack.push("picker");
                    host.calls.add("push returned (top is " + host.stack.top() + ")");
                }
            });
            host.stack.push("setup");
            equal(Arrays.asList("hidden launcher", "attach setup", "shown setup (top is setup, size 2)",
                    "push returned (top is setup)", "hidden setup", "attach picker",
                    "shown picker (top is picker, size 3)"), host.take(), "order");
            // The same for a pop asked for from inside onShown.
            host.whenShown.put("setup", new Runnable() {
                @Override public void run() {
                    host.stack.pop();
                }
            });
            host.stack.pop();
            equal(Arrays.asList("hidden picker", "detach picker", "shown setup (top is setup, size 2)", "hidden setup",
                    "detach setup", "shown launcher (top is launcher, size 1)"), host.take(), "order of the two pops");
        });

        run("ScreenStack.clearHidesTheTopAndDetachesAll", () -> {
            Recorder host = recorder();
            host.stack.push("launcher");
            host.stack.push("about");
            host.take();
            host.stack.clear();
            equal(Arrays.asList("hidden about", "detach about", "detach launcher"), host.take(), "clear");
            equal(0, host.stack.size(), "empty afterwards");
            equal(null, host.stack.top(), "no top afterwards");
        });

        run("ScreenStack.strayGamepadButtonsAndMadeUpKeysAreSwallowed", () -> {
            int fallback = 0x400;   // android.view.KeyEvent.FLAG_FALLBACK, written out from the Android documentation
            for (int code : new int[] {106, 107, 99, 109, 104, 105, 98, 101, 110, 188, 203}) {
                check(ScreenStack.ignoresKey(code, 0), "gamepad button " + code + " (stick click, X, Select, L2, R2, ...) is swallowed");
            }
            for (int code : new int[] {96, 97, 100, 102, 103, 108, 19, 20, 21, 22, 23, 66, 4, 111}) {
                check(!ScreenStack.ignoresKey(code, 0), "menu key " + code + " is not swallowed");
            }
            for (int code : new int[] {24, 25, 3, 26, 82, 95, 111, 187, 204}) {
                check(!ScreenStack.ignoresKey(code, 0), "key " + code + " (volume, Home, power, ...) is left to Android");
            }
            check(ScreenStack.ignoresKey(23, fallback), "DPAD_CENTER made up by Android for a stick click is swallowed");
            check(ScreenStack.ignoresKey(4, fallback | 0x8), "any made-up key is swallowed, whatever other flags it has");
            check(!ScreenStack.ignoresKey(23, 0x8 | 0x40), "a real DPAD_CENTER with other flags is not");
        });

        run("ScreenStack.gameRunsWhenItsProcessIsListedAndAlsoWhenAndroidGivesNoList", () -> {
            String app = "dev.recompbootstrap.smoke";
            check(ScreenStack.gameProcessListed(java.util.Arrays.asList(app, app + ":private_trace"), app), "listed: running");
            check(!ScreenStack.gameProcessListed(java.util.Arrays.asList(app), app), "only the launcher's own process: not running");
            check(!ScreenStack.gameProcessListed(new java.util.ArrayList<String>(), app), "an empty list: not running");
            check(!ScreenStack.gameProcessListed(java.util.Arrays.asList(app + ":private_trace2", "other:private_trace"), app),
                    "similar names do not count");
            check(ScreenStack.gameProcessListed(null, app), "no list from Android: treated as running, so that nothing is written on a guess");
        });

        LauncherTestSupport.finish("ScreenStackTest");
    }
}
