package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Looper;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PixelCopy;
import android.view.SurfaceView;
import android.widget.FrameLayout;
import android.widget.TextView;
import checksim.Sim;
import java.io.File;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * The game window on the host. It runs the app's REAL classes CaptureController, CaptureCombo, CaptureCensus,
 * CaptureStore, CaptureBadge, PrivateGamepadInput, XboxGamepadMapper, PhysicalMotionLog and ControllerCheckText,
 * compiled from the tree. The android.* classes they call are small stand-ins with one virtual clock for all
 * threads (folder fake/), and the activity's two native calls that feed the game are a stand-in that records every
 * call ("what the game receives"). The stand-ins and this rig were written by the checker of the ship integration
 * (lens input-and-capture) and are kept here so that the game window's input and capture have host tests.
 *
 * What is NOT the app's code here: Win.key / Win.motion / Win.focus repeat the three lines of
 * PrivateTraceActivity.dispatchKeyEvent, dispatchGenericMotionEvent and onWindowFocusChanged, and the constructor
 * repeats the two lines that make the gamepad input and then the capture hook. LauncherContractTest reads the
 * activity's source for those lines.
 *
 * What the rig cannot show: real races between the main thread and the capture thread (one clock, one message at
 * a time), and the order in which Android hands over an input event and a timer message that are due together
 * (here: the one that was due first).
 */
final class Rig {
    private Rig() {}

    static final int GP = 0x401, JOY = 0x1000010, PADSRC = GP | JOY;
    static final int DOWN = 0, UP = 1, MOVE = 2;
    static final int SELECT = 109, A = 96, B = 97, X = 99, Y = 100, START = 108, L1 = 102, L2 = 104, R2 = 105, DPAD_UP = 19;
    static final int AX = 0, AY = 1, AZ = 11, ARZ = 14, HATX = 15, HATY = 16, LT = 17, RT = 18;
    static final String REQUEST = "0123456789abcdef0123456789abcdef";
    static File scratch;   // set by the test's main
    static File files;
    private static int folder;

    static int passed, failed;
    static final List<String> failures = new ArrayList<>();

    interface Case { void run() throws Exception; }

    static void check(String name, Case test) {
        fresh();
        try {
            test.run();
            ++passed;
            System.out.println("PASS  " + name);
        } catch (Throwable problem) {
            ++failed;
            failures.add(name);
            System.out.println("FAIL  " + name + "\n      " + problem);
        }
    }

    static void expect(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    static void same(Object want, Object got, String what) {
        if (!String.valueOf(want).equals(String.valueOf(got))) throw new AssertionError(what + "\n        want: " + want + "\n        got:  " + got);
    }

    /** A new device, a new clock, a new files folder with files/private-trace in it. */
    static void fresh() {
        Sim.reset();
        Looper.resetForTest();
        InputDevice.DEVICES.clear();
        // As on every Android device: the built-in "Virtual" keyboard, device number -1. InputDevice.getDevice(-1)
        // answers with it, also while no controller is connected.
        InputDevice virtual = new InputDevice();
        virtual.id = -1;
        virtual.name = "Virtual";
        virtual.virtual = true;
        virtual.sources = 0x101;   // SOURCE_KEYBOARD
        for (int code : new int[] {19, 20, 21, 22}) virtual.keys.add(code);
        InputDevice.DEVICES.put(-1, virtual);
        TextView.last = null;
        Bitmap.resetForTest();
        PixelCopy.resetForTest();
        files = new File(scratch, "files-" + (++folder));
        deleteTree(files);   // an earlier run of a check may have left this folder
        new File(files, "private-trace").mkdirs();
    }

    static InputDevice pad(int id, String name) {
        InputDevice d = new InputDevice();
        d.id = id;
        d.name = name;
        d.sources = PADSRC;
        for (int axis : new int[] {AX, AY, AZ, ARZ, HATX, HATY}) d.axes.put(axis, new InputDevice.MotionRange(-1, 1, 0));
        d.axes.put(LT, new InputDevice.MotionRange(0, 1, 0));
        d.axes.put(RT, new InputDevice.MotionRange(0, 1, 0));
        for (int code : new int[] {19, 20, 21, 22, 96, 97, 99, 100, 102, 103, 104, 105, 106, 107, 108, 109, 110}) d.keys.add(code);
        InputDevice.DEVICES.put(id, d);
        return d;
    }

    /** The game window as PrivateTraceActivity wires it. hook: with the capture hook (the merged window); without: the window before it. */
    static final class Win {
        final Activity activity = new Activity();
        final SurfaceView surface;
        final FrameLayout content = new FrameLayout();
        PrivateGamepadInput gamepadInput;
        CaptureController capture;
        final List<String> toSuper = new ArrayList<>();

        Win(boolean debuggable, boolean hook) {
            activity.filesDir = files;
            activity.info.flags = debuggable ? 2 : 0;   // ApplicationInfo.FLAG_DEBUGGABLE
            activity.intent.extras.put("request_id", REQUEST);
            surface = new SurfaceView(activity);
            gamepadInput = new PrivateGamepadInput(activity);                                            // onCreate: the gamepad input
            if (hook) capture = CaptureController.attach(activity, surface, content, e -> gamepadInput != null && gamepadInput.key(e));   // onCreate: the capture hook
        }

        boolean key(KeyEvent event) {                                                                   // dispatchKeyEvent
            if (capture != null && capture.key(event)) return true;
            if (gamepadInput != null && gamepadInput.key(event)) return true;
            toSuper.add(Sim.now + " code=" + event.getKeyCode() + " action=" + event.getAction() + " device=" + event.getDeviceId());
            return false;
        }

        boolean motion(MotionEvent event) {                                                             // dispatchGenericMotionEvent
            return gamepadInput != null && gamepadInput.motion(event);
        }

        void focus(boolean focused) {                                                                   // onWindowFocusChanged, then the tree observer
            if (!focused && gamepadInput != null) gamepadInput.clear();
            activity.window.decor.observer.dispatch(focused);
        }

        void destroy() {                                                                                // onDestroy
            if (capture != null) { capture.close(); capture = null; }
            if (gamepadInput != null) { gamepadInput.close(); gamepadInput = null; }
        }

        /** A key event handled on the main thread at `handled`, with its own event time. */
        void keyAt(long handled, long eventTime, int code, int action, int repeat, int device) {
            Sim.enqueue(Looper.getMainLooper(), () -> key(new KeyEvent(eventTime, action, code, repeat, device, device < 0 ? GP : PADSRC)), handled);
        }

        void down(long t, int code, int device) { keyAt(t, t, code, DOWN, 0, device); }

        void up(long t, int code, int device) { keyAt(t, t, code, UP, 0, device); }

        void repeat(long t, int code, int count, int device) { keyAt(t, t, code, DOWN, count, device); }

        /** A joystick event: axis, value pairs. */
        void moveAt(long t, int device, float... pairs) {
            Map<Integer, Float> axes = new HashMap<>();
            for (int i = 0; i + 1 < pairs.length; i += 2) axes.put((int) pairs[i], pairs[i + 1]);
            Sim.enqueue(Looper.getMainLooper(), () -> motion(new MotionEvent(t, device, JOY, MOVE, axes)), t);
        }

        void at(long t, Runnable r) { Sim.enqueue(Looper.getMainLooper(), r, t); }

        void removeDevice(long t, int id) {
            at(t, () -> { InputDevice.DEVICES.remove(id); activity.inputManager.removed(id); });
        }

        void addDevice(long t, int id, String name) {
            at(t, () -> { pad(id, name); activity.inputManager.added(id); });
        }
    }

    private static final int[] BITS = {0x20, 0x1000, 0x2000, 0x4000, 0x8000, 0x10, 0x100, 1, 2, 4, 8, 0x200, 0x40, 0x80, 0x400};
    private static final String[] NAMES = {"S", "A", "B", "X", "Y", "START", "LB", "UP", "DN", "LF", "RG", "RB", "L3", "R3", "MODE"};

    /** What the game was given: every change of a button bit, as "S+@250" (pressed at 250 ms) or "S-@1250". */
    static String edges() {
        StringBuilder out = new StringBuilder();
        int before = 0;
        for (long[] g : Sim.game) {
            int now = (int) g[2];
            for (int i = 0; i < BITS.length; ++i) {
                if (((now ^ before) & BITS[i]) != 0) out.append(NAMES[i]).append((now & BITS[i]) != 0 ? '+' : '-').append('@').append(g[0]).append(' ');
            }
            before = now;
        }
        return out.toString().trim();
    }

    static long[] last() { return Sim.game.get(Sim.game.size() - 1); }

    static int buttonsNow() { return (int) last()[2]; }

    /** The log lines that contain `part`, in order. */
    static List<String> log(String part) {
        List<String> out = new ArrayList<>();
        for (String line : Sim.log) if (line.contains(part)) out.add(line);
        return out;
    }

    static int count(String part) { return log(part).size(); }

    /** Every file and folder under `root`, relative path -> size (folders: -1), sorted. */
    static TreeMap<String, Long> tree(File root) {
        TreeMap<String, Long> out = new TreeMap<>();
        walk(root, "", out);
        return out;
    }

    private static void walk(File dir, String prefix, TreeMap<String, Long> out) {
        File[] list = dir.listFiles();
        if (list == null) return;
        for (File f : list) {
            String name = prefix + f.getName();
            if (f.isDirectory()) { out.put(name + "/", -1L); walk(f, name + "/", out); }
            else out.put(name, f.length());
        }
    }

    /** What the capture badge showed, in order: "<time> <text>" for every time it became visible. */
    static List<String> shown() {
        List<String> out = new ArrayList<>();
        if (TextView.last != null) for (String s : TextView.last.shown) if (s.contains(" SHOW ")) out.add(s.replace(" SHOW ", " "));
        return out;
    }

    static List<String> pictures() {
        List<String> out = new ArrayList<>();
        for (String name : tree(new File(files, "captures")).keySet()) if (name.endsWith(".png")) out.add(name);
        return out;
    }

    static void deleteTree(File f) {
        File[] list = f.listFiles();
        if (list != null) for (File c : list) deleteTree(c);
        f.setWritable(true);
        f.delete();
    }
}
