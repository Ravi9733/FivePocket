package dev.recompbootstrap.smoke;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;

/**
 * The Android glue of the menu-input-fixes (vm-pad-hold D1, D3, D4, D5), read from the sources because it cannot run
 * on a host: the activity, the screen and the view must hand their events to the tested plain-Java parts.
 * Argument: the folder with the app's Java sources (BootstrapActivity.java, LauncherScreen.java, UiKit.java).
 */
public final class MenuInputGlueTest {
    private static final String S = "MenuInputGlue";

    private static String read(String folder, String name) {
        try {
            return new String(Files.readAllBytes(Paths.get(folder, name)), StandardCharsets.UTF_8);
        } catch (IOException missing) {
            throw new AssertionError("cannot read " + name + ": " + missing);
        }
    }

    /** The text of the first block that starts with head, up to its matching closing brace. */
    private static String block(String source, String head) {
        int start = source.indexOf(head);
        MenuFixKit.check(start >= 0, "found: " + head);
        int open = source.indexOf('{', start), depth = 0;
        for (int i = open; i < source.length(); ++i) {
            char c = source.charAt(i);
            if (c == '{') depth++;
            else if (c == '}' && --depth == 0) return source.substring(start, i + 1);
        }
        throw new AssertionError("unclosed: " + head);
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        if (args.length < 1) throw new IllegalArgumentException("usage: MenuInputGlueTest <app java source folder>");
        final String folder = args[0];
        final String activity = read(folder, "BootstrapActivity.java");
        MenuFixKit.test(S, "D1: the self-test's surface callbacks and lifecycle make no native call; NativeBridge is called only from SelfTestNative", () -> {
            String callback = block(activity, "private final class SmokeSurfaceCallback");
            MenuFixKit.check(!callback.contains("NativeBridge."), "SmokeSurfaceCallback calls NativeBridge");
            String bridge = block(activity, "private final class SelfTestNative");
            // configureDiagnostic (the debug build's picture check) calls the native side too: from SelfTestNative only.
            String diagnostic = block(activity, "private void configureDiagnostic()");
            String rest = activity.replace(bridge, "").replace(diagnostic, "");
            MenuFixKit.check(!rest.contains("NativeBridge."), "a NativeBridge call outside SelfTestNative");
            MenuFixKit.check(!rest.contains("configureDiagnostic()") && bridge.contains("configureDiagnostic()"),
                    "configureDiagnostic is called from SelfTestNative only");
            for (String step : new String[] {"selfTest.surfaceCreated(", "selfTest.surfaceChanged(", "selfTest.surfaceDestroyed()"}) {
                MenuFixKit.check(callback.contains(step), "the callback hands over: " + step);
            }
            MenuFixKit.check(block(activity, "protected void onCreate(").contains("selfTest.start()"), "onCreate starts the run");
            MenuFixKit.check(block(activity, "protected void onDestroy(").contains("selfTest.leave()"), "onDestroy leaves it");
            MenuFixKit.check(activity.contains("SelfTestRun.processWorker()"), "the process's one worker is used");
        });
        MenuFixKit.test(S, "D3: every sample of a joystick event goes to NavInput.motion; joystick input is unbuffered", () -> {
            String motion = block(activity, "boolean motion(final MotionEvent event)");
            MenuFixKit.check(motion.contains("navInput.motion(") && !motion.contains("navInput.axes("), "motion() uses NavInput.motion");
            MenuFixKit.check(motion.contains("getHistorySize()") && motion.contains("getHistoricalAxisValue(axis, position)"), "the batched samples are read");
            MenuFixKit.check(activity.contains("requestUnbufferedDispatch(InputDevice.SOURCE_CLASS_JOYSTICK)"), "unbuffered joystick dispatch");
        });
        MenuFixKit.test(S, "M3: keys and joystick samples reach NavInput with their own times", () -> {
            MenuFixKit.check(block(activity, "boolean key(KeyEvent event)").contains("event.getRepeatCount(), event.getEventTime())"), "the key's time");
            String motion = block(activity, "boolean motion(final MotionEvent event)");
            MenuFixKit.check(motion.contains("event.getHistoricalEventTime(position) : event.getEventTime()"), "the samples' times");
        });
        MenuFixKit.test(S, "M2: a question or a save a command opened ends the hold (LauncherScreen.onNav)", () -> {
            String onNav = block(read(folder, "LauncherScreen.java"), "@Override public boolean onNav(int command, boolean repeat)");
            MenuFixKit.check(onNav.contains("boolean asked = settings.capturing();") && onNav.contains("if (!asked && settings.capturing()) host.endHold();"),
                    "onNav ends the hold when a question opened");
            MenuFixKit.check(onNav.indexOf("handle(command, repeat);") < onNav.indexOf("if (!asked"), "after the command was carried out");
        });
        MenuFixKit.test(S, "minor 2 and 4: a pad taken away resets NavInput; the diagnostic heading is volatile", () -> {
            MenuFixKit.check(block(activity, "public void onInputDeviceRemoved(int id)").contains("navInput.reset();"), "removal resets");
            MenuFixKit.check(block(activity, "void create(String savedAwaiting)").contains("registerInputDeviceListener(padRemoved, handler)"), "registered");
            MenuFixKit.check(block(activity, "        void destroy() {").contains("unregisterInputDeviceListener(padRemoved)"), "unregistered");
            MenuFixKit.check(activity.contains("private volatile String diagnosticHeading;"), "volatile");
        });
        MenuFixKit.test(S, "D4: the scene view draws only a scene that differs from the one shown", () -> {
            String show = block(read(folder, "UiKit.java"), "void show(Scene next)");
            MenuFixKit.check(show.contains("if (!Scene.needsDraw(scene, next)) return;"), "show() asks Scene.needsDraw first");
            MenuFixKit.check(show.indexOf("needsDraw") < show.indexOf("invalidate()"), "before invalidate()");
        });
        MenuFixKit.test(S, "D5: a tab change by L1 / R1 ends the hold through the host", () -> {
            String screen = read(folder, "LauncherScreen.java");
            String tab = screen.substring(screen.indexOf("case ShellNav.TAB:"), screen.indexOf("case ShellNav.ENTERED:"));
            MenuFixKit.check(tab.contains("if (ShellNav.endsHold(answer, command)) host.endHold();"), "carryOut's TAB case");
            MenuFixKit.check(block(screen, "void settingsLeft(int answer)").contains("host.endHold()"), "the settings question's tab change");
            MenuFixKit.check(block(activity, "public void endHold()").contains("navInput.endHold()"), "the host passes it to NavInput");
        });
        MenuFixKit.finish(S);
    }
}
