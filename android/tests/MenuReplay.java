package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;

/**
 * The checker's replay harness (menu-input-fixes/check/harness/Replay.java), taken into the tests as a regression
 * (menu-input-fixes, second round). Its 127 scripts are unchanged; scripts named "lane/..." were added after them.
 * The same file is compiled against today's sources (replay/today/MenuReplayGlue.java, outside the tree: it makes the
 * "today" column of MenuReplayExpected) and against the lane's (tests/MenuReplayGlue.java); only the glue differs. It
 * mirrors the lines of BootstrapActivity (key, motion, pad removal) and LauncherScreen (onNav, carryOut, settingsLeft)
 * that hand input to NavInput. Changes to the checker's plumbing: keys and samples carry their time, a pad can be
 * taken away (Script.removed), and onNav tells the glue whether a question opened. The menu side mirrors LauncherScreen.onNav / carryOut and SettingsScreen
 * (command, paneCommand, enter, askLeave, settle, save, load) synchronously: a save always succeeds at once, a load
 * is the saved settings at once. ABOUT's Licence pane is a clamped scroll counter (AboutScreen is not used).
 *
 * Physical input: HAT axes (the virtual pad's D-pad), the left stick, and buttons (keys). Motion samples are batched
 * into one MotionEvent per frame (frame period per script, default 16 ms) and delivered at the frame boundary; keys
 * are delivered at their time. Mode "unbuffered" (lane only) delivers every motion sample as its own event at once.
 *
 * Output per script: one line "END <script> <state>" and one line "CMD <script> <commands>".
 */
public final class MenuReplay {
    static final int KEYS_A = 96, KEY_B = 97, KEY_L1 = 102, KEY_R1 = 103, KEY_START = 108, KEY_Y = 100,
            KEY_UP = 19, KEY_DOWN = 20, KEY_LEFT = 21, KEY_RIGHT = 22;
    static final String[] NAMES = {"NONE", "UP", "DOWN", "LEFT", "RIGHT", "A", "B", "L1", "R1", "START", "Y"};

    // ------------------------------------------------------------------------------------------ timer

    static final class Clock implements NavInput.Timer {
        long now;
        Runnable task;
        long due;

        @Override public long now() { return now; }
        @Override public void schedule(Runnable t, long delayMs) { task = t; due = now + delayMs; }
        @Override public void cancel() { task = null; }

        /** Runs due tasks up to time (inclusive), then sets now = time. */
        void runTo(long time) {
            while (task != null && due <= time) {
                now = due;
                Runnable t = task;
                task = null;
                t.run();
            }
            now = Math.max(now, time);
        }
    }

    // ------------------------------------------------------------------------------------------ the menu

    static final class Menu implements NavInput.Sink {
        final Clock clock = new Clock();
        final NavInput input = new NavInput(this, clock);
        final ShellNav nav = new ShellNav();
        final SettingsPages pages = new SettingsPages();
        final Capabilities caps = SettingsCoreTestKit.caps(SettingsCoreTestKit.ALL_NAMES);
        MenuModel model;
        Settings saved = Settings.defaults();
        int saves;
        int licence;   // Licence pane scroll position (0..LICENCE_END)
        static final int LICENCE_END = 20;
        final List<String> commands = new ArrayList<String>();
        final List<String> opened = new ArrayList<String>();

        Menu() {
            nav.setColumnCount(ShellNav.PLAY, 1);
            nav.setColumnCount(ShellNav.SETTINGS, SettingsPages.COUNT);
            nav.setColumnCount(ShellNav.CONTROLLER, 1);
            nav.setColumnCount(ShellNav.ABOUT, 5);
            load();
        }

        void load() {
            model = new MenuModel();
            model.loaded(saved, true, new ArrayList<String>(), new ArrayList<String>(), caps);
            pages.reloaded(model, nav);
        }

        boolean capturing() {
            return model.overlay() != MenuModel.OVERLAY_NONE || model.pendingAction() != MenuModel.ACTION_NONE;
        }

        @Override public void onNav(int command, boolean repeat) {
            commands.add(NAMES[command] + (repeat ? "+" : "") + "@" + clock.now);
            boolean asked = capturing();
            handle(command, repeat);
            MenuReplayGlue.afterCommand(input, asked, capturing());   // LauncherScreen.onNav
        }

        void handle(int command, boolean repeat) {
            int tab = nav.tab();
            int route = ShellNav.route(tab, command, capturing(), model.placeholder() == null, false);
            if (route == ShellNav.TO_SETTINGS_MODEL) {
                pages.forward(model, nav, command, repeat);
                settle();
                return;
            }
            carryOut(tab, nav.command(command, repeat, enterable(tab), model.dirty()), command, repeat);
        }

        boolean enterable(int tab) {
            if (tab == ShellNav.SETTINGS) return model.placeholder() == null && SettingsPages.rows(model, nav.column(ShellNav.SETTINGS)).length > 0;
            return tab == ShellNav.ABOUT && nav.column(ShellNav.ABOUT) == 2;
        }

        void carryOut(int tab, int answer, int command, boolean repeat) {
            switch (answer) {
                case ShellNav.TAB:
                    MenuReplayGlue.tabChanged(input, answer, command);
                    entered(nav.tab());
                    break;
                case ShellNav.ENTERED:
                    if (tab == ShellNav.SETTINGS) {
                        int category = nav.column(ShellNav.SETTINGS);
                        if (!pages.enter(model, category)) nav.place(ShellNav.SETTINGS, category, false);
                    }
                    break;
                case ShellNav.ACTIVATE:
                    opened.add(ShellNav.TAB_NAMES[tab] + "/" + nav.column(tab) + "@" + clock.now);
                    input.reset();   // host.push resets NavInput
                    break;
                case ShellNav.PANE_COMMAND:
                    if (tab == ShellNav.SETTINGS) {
                        if (command == NavInput.UP || command == NavInput.DOWN) pages.move(model, command == NavInput.UP ? -1 : 1, repeat);
                        else if (command == NavInput.LEFT || command == NavInput.RIGHT || command == NavInput.CONFIRM) {
                            pages.forward(model, nav, command, repeat);
                            settle();
                        }
                    } else if (tab == ShellNav.ABOUT) {
                        if (command == NavInput.UP) licence = Math.max(0, licence - 1);
                        else if (command == NavInput.DOWN) licence = Math.min(LICENCE_END, licence + 1);
                        else if (command == NavInput.LEFT) nav.place(ShellNav.ABOUT, nav.column(ShellNav.ABOUT), false);
                    }
                    break;
                case ShellNav.ASK:
                    model.nav(NavInput.BACK, false);
                    settle();
                    break;
                default:
                    break;
            }
        }

        void entered(int tab) {
            if (tab == ShellNav.SETTINGS && !model.dirty() && !capturing()) load();
        }

        void settle() {
            int action = model.pendingAction();
            if (action == MenuModel.ACTION_SAVE) {
                boolean onDrift = SettingsPages.onDriftRow(model);
                saved = model.beginSave();
                saves++;
                model.saveFinished(true, false, null);
                pages.afterSave(model, nav, onDrift);
                settle();
                return;
            }
            if (action == MenuModel.ACTION_LEAVE) {
                load();
                int answer = nav.leaveGranted();
                MenuReplayGlue.settingsLeft(input, answer);
                if (answer == ShellNav.TAB) entered(nav.tab());
                return;
            }
            if (nav.asking() && !capturing()) nav.leaveCancelled();
        }

        String state() {
            StringBuilder s = new StringBuilder();
            s.append("tab=").append(ShellNav.TAB_NAMES[nav.tab()]).append(" level=").append(nav.level());
            s.append(" cols=");
            for (int t = 0; t < ShellNav.TABS; ++t) s.append(nav.column(t)).append(nav.inPane(t) ? "p" : "").append(t < 3 ? "," : "");
            s.append(" focus=").append(model.rowLabel(model.focused()));
            s.append(" overlay=").append(model.overlay());
            if (model.overlay() != MenuModel.OVERLAY_NONE) s.append(":").append(model.overlayButtonLabel(model.overlayFocused()));
            s.append(" dirty=").append(model.dirty());
            s.append(" values=[");
            for (int i = 0; i < model.rowCount(); ++i) {
                if (model.rowKey(i) != null) s.append(model.rowLabel(i)).append('=').append(model.rowValue(i)).append(';');
            }
            s.append("] saves=").append(saves).append(" saved=").append(saved.equals(Settings.defaults()) ? "defaults" : saved.toString());
            s.append(" licence=").append(licence).append(" opened=").append(opened);
            return s.toString();
        }
    }

    // ------------------------------------------------------------------------------------------ scripts

    static final class Script {
        final String name;
        final List<long[]> keys = new ArrayList<long[]>();      // time, code, down
        final List<float[]> samples = new ArrayList<float[]>(); // time, hx, hy, x, y (full state after a change)
        final List<long[]> frames = new ArrayList<long[]>();    // from, to, period
        final List<Long> removals = new ArrayList<Long>();      // a pad taken away (InputDeviceListener.onInputDeviceRemoved)
        float hx, hy, x, y;
        long end;
        boolean noTail;   // no final "all released" sample (a pad that disconnected)

        Script(String name) { this.name = name; }

        Script hat(long t, float nx, float ny) { hx = nx; hy = ny; sample(t); return this; }
        Script stick(long t, float nx, float ny) { x = nx; y = ny; sample(t); return this; }
        Script both(long t, float nhx, float nhy, float nx, float ny) { hx = nhx; hy = nhy; x = nx; y = ny; sample(t); return this; }
        private void sample(long t) { samples.add(new float[]{t, hx, hy, x, y}); end = Math.max(end, t); }
        Script key(long t, int code, boolean down) { keys.add(new long[]{t, code, down ? 1 : 0}); end = Math.max(end, t); return this; }
        Script tap(long t, int code, long dur) { return key(t, code, true).key(t + dur, code, false); }
        /** A held HAT direction: dx, dy in -1..1 from t for dur ms. */
        Script hatHold(long t, float dx, float dy, long dur) { return hat(t, dx, dy).hat(t + dur, 0, 0); }
        Script stickHold(long t, float sx, float sy, long dur) { return stick(t, sx, sy).stick(t + dur, 0, 0); }
        Script slow(long from, long to, long period) { frames.add(new long[]{from, to, period}); return this; }
        Script removed(long t) { removals.add(t); end = Math.max(end, t); return this; }

        long period(long t) {
            for (long[] f : frames) if (t >= f[0] && t < f[1]) return f[2];
            return 16;
        }

        long boundary(long t) {
            long p = period(t);
            for (long[] f : frames) if (t >= f[0] && t < f[1]) return f[0] + ((t - f[0]) / p + 1) * p;
            return (t / p + 1) * p;
        }
    }

    /** Delivers a script to a fresh menu; returns the menu. */
    static Menu run(Script s, boolean unbuffered) {
        Menu menu = new Menu();
        // Build the delivery list: [time, kind(0 key, 1 motion), index...]
        List<Object[]> deliveries = new ArrayList<Object[]>();
        for (long[] k : s.keys) deliveries.add(new Object[]{k[0], 0, k});
        for (Long t : s.removals) deliveries.add(new Object[]{t, 2, null});
        if (unbuffered) {
            for (float[] smp : s.samples) {
                List<float[]> one = new ArrayList<float[]>();
                one.add(smp);
                deliveries.add(new Object[]{(long) smp[0], 1, one});
            }
        } else {
            List<float[]> batch = null;
            long at = -1;
            for (float[] smp : s.samples) {
                long b = s.boundary((long) smp[0]);
                if (batch == null || b != at) {
                    batch = new ArrayList<float[]>();
                    at = b;
                    deliveries.add(new Object[]{b, 1, batch});
                }
                batch.add(smp);
            }
        }
        // Stable by time; a key at the same time as a motion boundary goes first (it is not batched).
        deliveries.sort((a, b) -> {
            int c = Long.compare((Long) a[0], (Long) b[0]);
            return c != 0 ? c : Integer.compare((Integer) a[1], (Integer) b[1]);
        });
        for (Object[] d : deliveries) {
            long t = (Long) d[0];
            menu.clock.runTo(t);
            if ((Integer) d[1] == 0) {
                long[] k = (long[]) d[2];
                // A key held down repeats in Android with repeatCount > 0 (every ~50 ms); NavInput ignores them.
                MenuReplayGlue.key(menu.input, (int) k[1], k[2] == 1, k[0]);
            } else if ((Integer) d[1] == 2) {
                MenuReplayGlue.removed(menu.input);
            } else {
                @SuppressWarnings("unchecked") List<float[]> batch = (List<float[]>) d[2];
                MenuReplayGlue.deliver(menu.input, batch);
            }
        }
        menu.clock.runTo(s.end + 3000);
        return menu;
    }

    // ------------------------------------------------------------------------------------------ the scripts

    static final float F = 1f;

    /** Helpers that bring the menu to a place with clean single presses (the same in both trees). */
    static Script start(String name, String where) {
        Script s = new Script(name);
        long t = 0;
        switch (where) {
            case "strip": s.tap(t, KEY_B, 80); break;                                   // PLAY column -> strip
            case "play-column": break;
            case "settings-column": s.tap(t, KEY_R1, 80); break;                        // SETTINGS, category Display
            case "display-rows": s.tap(0, KEY_R1, 80).tap(200, KEYS_A, 80); break;     // into Display rows (Scaler)
            case "framerate-rows": s.tap(0, KEY_R1, 80).hatHold(200, 0, F, 80).tap(400, KEYS_A, 80); break;
            case "world-rows": s.tap(0, KEY_R1, 80).hatHold(200, 0, F, 80).hatHold(400, 0, F, 80).tap(600, KEYS_A, 80); break;
            case "controls-rows": s.tap(0, KEY_R1, 80).hatHold(200, 0, F, 80).hatHold(400, 0, F, 80).hatHold(600, 0, F, 80).tap(800, KEYS_A, 80); break;
            case "saverestore-rows": s.tap(0, KEY_R1, 80).hatHold(200, 0, -F, 80).tap(400, KEYS_A, 80); break;  // DOWN wraps? UP from Display: strip
            case "controller-column": s.tap(0, KEY_L1, 80).tap(200, KEY_L1, 80); break;  // PLAY -> ABOUT -> CONTROLLER
            case "about-column": s.tap(0, KEY_L1, 80); break;
            case "licence": s.tap(0, KEY_L1, 80).hatHold(200, 0, F, 80).hatHold(400, 0, F, 80).hatHold(600, F, 0, 80); break;
            case "world-dirty":   // World Traffic LEFT x1 (95 %), then B to the category name
                s.tap(0, KEY_R1, 80).hatHold(200, 0, F, 80).hatHold(400, 0, F, 80).tap(600, KEYS_A, 80)
                        .hatHold(800, -F, 0, 80).tap(1000, KEY_B, 80);
                break;
            case "world-dirty-rows":   // the same, still in the rows
                s.tap(0, KEY_R1, 80).hatHold(200, 0, F, 80).hatHold(400, 0, F, 80).tap(600, KEYS_A, 80).hatHold(800, -F, 0, 80);
                break;
            default: throw new IllegalArgumentException(where);
        }
        return s;
    }

    static final long T0 = 2000;   // scripts start their own input here

    static List<Script> scripts() {
        List<Script> all = new ArrayList<Script>();
        // ---- from vm-pad-hold's page folders (kernel hold times as recorded there)
        all.add(start("vmh/tabs/01 B on PLAY column", "play-column").tap(T0, KEY_B, 120));
        all.add(start("vmh/tabs/02 RIGHT held 2532 on strip", "strip").hatHold(T0, F, 0, 2532));
        all.add(start("vmh/tabs/03 RIGHT 2532 then LEFT 2688", "strip").hatHold(T0, F, 0, 2532).hatHold(T0 + 4000, -F, 0, 2688));
        all.add(start("vmh/tabs/05 RB held 2078", "strip").tap(T0, KEY_R1, 2078));
        all.add(start("vmh/tabs/06 LB held 2047 from SETTINGS strip", "strip").tap(T0, KEY_R1, 100).tap(T0 + 500, KEY_L1, 2047));
        all.add(start("vmh/tabs/07 LB on PLAY wraps", "strip").tap(T0, KEY_L1, 100));
        all.add(start("vmh/tabs/08 RB on ABOUT wraps", "strip").tap(T0, KEY_L1, 100).tap(T0 + 500, KEY_R1, 100));
        all.add(start("vmh/tabs/09 stick x 0.50 5074", "strip").stickHold(T0, 0.50f, 0, 5074));
        all.add(start("vmh/tabs/10 stick x 0.52 3061", "strip").stickHold(T0, 0.52f, 0, 3061));
        all.add(start("vmh/tabs/11 stick x 0.58 398", "strip").stickHold(T0, 0.58f, 0, 398));
        all.add(start("vmh/tabs/13 stick x 0.58 420", "strip").stickHold(T0, 0.58f, 0, 420));
        all.add(start("vmh/tabs/12 stick x -0.58 66 (slow frame 300)", "strip").tap(T0 - 500, KEY_R1, 80)
                .slow(T0 - 100, T0 + 1000, 300).stickHold(T0 + 10, -0.58f, 0, 66));
        all.add(start("vmh/tabs/14 stick 0.6 then 0.4 1 ms apart", "strip").stick(T0, 0.6f, 0).stick(T0 + 1, 0.4f, 0).stick(T0 + 1500, 0, 0));
        all.add(start("vmh/tabs/15 HAT down+right 1591", "strip").hatHold(T0, F, F, 1591));
        all.add(start("vmh/tabs/16 stick (1,1) 370 on strip", "strip").stickHold(T0, F, F, 370));
        all.add(start("vmh/play/01 UP held 1553 on PLAY column", "play-column").hatHold(T0, 0, -F, 1553));
        all.add(start("vmh/cross/02 DOWN 3260, RB at 540 (CONTROLLER)", "controller-column").hatHold(T0, 0, F, 3260).tap(T0 + 540, KEY_R1, 90));
        all.add(start("vmh/cross/04 HAT DOWN 4800, stick UP -0.8 750..2280", "settings-column").hatHold(T0, 0, F, 4800).stickHold(T0 + 750, 0, -0.8f, 1530));
        all.add(start("vmh/display/01 DOWN held 2094 in rows", "display-rows").hatHold(T0, 0, F, 2094));
        all.add(start("vmh/display/03 DOWN 2094 then UP 2036", "display-rows").hatHold(T0, 0, F, 2094).hatHold(T0 + 3000, 0, -F, 2036));
        all.add(start("vmh/display/04 LEFT held 2165 on Scaler", "display-rows").hatHold(T0, -F, 0, 2165));
        all.add(start("vmh/display/05 RIGHT held 1549 on Scaler", "display-rows").hatHold(T0, F, 0, 1549));
        all.add(start("vmh/display/06 RIGHT 1549 then LEFT 3036", "display-rows").hatHold(T0, F, 0, 1549).hatHold(T0 + 2500, -F, 0, 3036));
        all.add(start("vmh/display/07 stick down 0.6 2000", "display-rows").stickHold(T0, 0, 0.6f, 2000));
        all.add(start("vmh/display/08 stick down 0.6 then up 1.0", "display-rows").stickHold(T0, 0, 0.6f, 2000).stickHold(T0 + 3000, 0, -F, 2000));
        all.add(start("vmh/display/09 stick (1,1) 275 on Scaler", "display-rows").stickHold(T0, F, F, 275));
        all.add(start("vmh/display/11 RIGHT held 2125 on category name", "settings-column").hatHold(T0, F, 0, 2125));
        all.add(start("vmh/display/13 stick right 1.0 2039 on category name", "settings-column").stickHold(T0, F, 0, 2039));
        all.add(start("vmh/framerate/01 two DOWN taps 359/322 in 1 s frames", "framerate-rows").slow(T0 - 50, T0 + 2500, 1000)
                .hatHold(T0, 0, F, 359).hatHold(T0 + 500, 0, F, 322));
        all.add(start("vmh/framerate/04 DOWN 176 then DOWN", "framerate-rows").hatHold(T0, 0, F, 176).hatHold(T0 + 600, 0, F, 120));
        all.add(start("vmh/framerate/06 A held 2066 on Counter", "framerate-rows").hatHold(T0, 0, F, 120).hatHold(T0 + 400, 0, F, 120).tap(T0 + 800, KEYS_A, 2066));
        all.add(start("vmh/framerate/08 B held 2025 from rows", "framerate-rows").tap(T0, KEY_B, 2025));
        all.add(start("vmh/restore/01 Y held 2046 then B 2026", "framerate-rows").tap(T0, KEY_Y, 2046).tap(T0 + 3000, KEY_B, 2026));
        all.add(start("vmh/world/01 RIGHT held 1526 on Traffic max", "world-rows").hatHold(T0, F, 0, 1526));
        all.add(start("vmh/world/03 LEFT held 1040 on Traffic", "world-rows").hatHold(T0, -F, 0, 1040));
        all.add(start("vmh/save/02 Traffic 65, B, B held 2040", "world-rows").hatHold(T0, -F, 0, 1040).tap(T0 + 1500, KEY_B, 100).tap(T0 + 2000, KEY_B, 2040));
        all.add(start("vmh/save/03 question, RIGHT held 1545", "world-dirty").tap(T0, KEY_B, 100).hatHold(T0 + 500, F, 0, 1545));
        all.add(start("vmh/save/04 question, RIGHT 1545 then LEFT 1596", "world-dirty").tap(T0, KEY_B, 100).hatHold(T0 + 500, F, 0, 1545).hatHold(T0 + 2500, -F, 0, 1596));
        all.add(start("vmh/save/05 question, stick right 1.0 1823", "world-dirty").tap(T0, KEY_B, 100).stickHold(T0 + 500, F, 0, 1823));
        all.add(start("vmh/save/06 question, RIGHT 1545, A held 2044", "world-dirty").tap(T0, KEY_B, 100).hatHold(T0 + 500, F, 0, 1545).tap(T0 + 2500, KEYS_A, 2044));
        all.add(start("vmh/column/01 DOWN 3035 then UP 2542 (settings column)", "settings-column").hatHold(T0, 0, F, 3035).hatHold(T0 + 4000, 0, -F, 2542));
        all.add(start("vmh/controls/01 DOWN 1568 in Controls", "controls-rows").hatHold(T0, 0, F, 1568));
        all.add(start("vmh/controller-tab/01 A held 2143 on strip", "strip").tap(T0, KEY_R1, 80).tap(T0 + 300, KEY_R1, 80).tap(T0 + 600, KEYS_A, 2143));
        all.add(start("vmh/about/rate DOWN 331 in Licence", "licence").hatHold(T0, 0, F, 331));
        all.add(start("vmh/about/rate DOWN 928 in Licence", "licence").hatHold(T0, 0, F, 928));
        all.add(start("vmh/about/rate DOWN 1519 in Licence", "licence").hatHold(T0, 0, F, 1519));
        all.add(start("vmh/about/05 DOWN 5111 then UP 7071 in Licence", "licence").hatHold(T0, 0, F, 5111).hatHold(T0 + 6000, 0, -F, 7071));
        all.add(start("vmh/about/11 DOWN 2909 from strip into ABOUT column", "strip").tap(T0, KEY_L1, 80).hatHold(T0 + 300, 0, F, 2909));

        // ---- invented
        all.add(start("inv/LB during held DOWN on SETTINGS column", "settings-column").hatHold(T0, 0, F, 2000).tap(T0 + 300, KEY_L1, 80));
        all.add(start("inv/RB during held LEFT on Traffic (dirty -> question)", "world-rows").hatHold(T0, -F, 0, 700).tap(T0 + 450, KEY_R1, 60).tap(T0 + 1200, KEYS_A, 80));
        all.add(start("inv/RB during held LEFT on Traffic, long", "world-rows").hatHold(T0, -F, 0, 1600).tap(T0 + 1300, KEY_R1, 60).tap(T0 + 2000, KEYS_A, 80));
        all.add(start("inv/RB during held RIGHT (Traffic dirty) -> question", "world-dirty-rows").hatHold(T0, F, 0, 700).tap(T0 + 450, KEY_R1, 60).tap(T0 + 1200, KEYS_A, 80));
        all.add(start("inv/B twice during held LEFT on Traffic", "world-rows").hatHold(T0, -F, 0, 1200).tap(T0 + 450, KEY_B, 60).tap(T0 + 700, KEY_B, 60).tap(T0 + 1500, KEYS_A, 80));
        all.add(start("inv/B question then A at once", "world-dirty").tap(T0, KEY_B, 80).tap(T0 + 300, KEYS_A, 80));
        all.add(start("inv/B question then START", "world-dirty").tap(T0, KEY_B, 80).tap(T0 + 300, KEY_START, 80));
        all.add(start("inv/B question then LEFT A", "world-dirty").tap(T0, KEY_B, 80).hatHold(T0 + 300, -F, 0, 80).tap(T0 + 600, KEYS_A, 80));
        all.add(start("inv/RB with dirty settings -> question, A", "world-dirty").tap(T0, KEY_R1, 80).tap(T0 + 300, KEYS_A, 80));
        all.add(start("inv/stick 40deg 0.8 on Scaler", "display-rows").stickHold(T0, 0.613f, 0.514f, 300));
        all.add(start("inv/stick 38deg 0.89 on Scaler", "display-rows").stickHold(T0, 0.70f, 0.55f, 300));
        all.add(start("inv/stick 30deg full on Scaler", "display-rows").stickHold(T0, 0.866f, 0.5f, 300));
        all.add(start("inv/stick 50deg full on Scaler", "display-rows").stickHold(T0, 0.643f, 0.766f, 300));
        all.add(start("inv/stick 40deg full on strip", "strip").stickHold(T0, 0.766f, 0.643f, 300));
        all.add(start("inv/stick ramp through diagonal to right", "display-rows").stick(T0, 0.3f, 0.3f).stick(T0 + 8, 0.6f, 0.6f).stick(T0 + 16, 0.9f, 0.4f).stick(T0 + 24, F, 0).stick(T0 + 400, 0, 0));
        all.add(start("inv/stick ramp through diagonal in one slow frame", "display-rows").slow(T0 - 10, T0 + 500, 250)
                .stick(T0, 0.3f, 0.3f).stick(T0 + 8, 0.6f, 0.6f).stick(T0 + 16, 0.9f, 0.4f).stick(T0 + 24, F, 0).stick(T0 + 400, 0, 0));
        all.add(start("inv/HAT DOWN and stick DOWN together then stick lets go", "licence").both(T0, 0, F, 0, F).stick(T0 + 1000, 0, 0).hat(T0 + 2500, 0, 0));
        all.add(start("inv/HAT DOWN held, stick RIGHT on Traffic, stick lets go", "world-rows").hat(T0, 0, F).stickHold(T0 + 100, F, 0, 300).hat(T0 + 2000, 0, 0));
        all.add(start("inv/HAT LEFT held on Traffic, stick UP, stick lets go", "world-rows").hat(T0, -F, 0).stickHold(T0 + 100, 0, -F, 300).hat(T0 + 2000, 0, 0));
        all.add(start("inv/stick DOWN held, HAT DOWN tap under it", "licence").stick(T0, 0, F).hatHold(T0 + 500, 0, F, 100).stick(T0 + 2000, 0, 0));
        all.add(start("inv/A during a DOWN repeat in Licence", "licence").hatHold(T0, 0, F, 1500).tap(T0 + 700, KEYS_A, 80));
        all.add(start("inv/A during DOWN repeat on PLAY column (opens)", "play-column").hatHold(T0, 0, F, 1500).tap(T0 + 700, KEYS_A, 80));
        all.add(start("inv/two taps in one 1 s frame on strip", "strip").slow(T0 - 50, T0 + 2000, 1000).hatHold(T0, F, 0, 100).hatHold(T0 + 300, F, 0, 100));
        all.add(start("inv/tap shorter than a frame 5 ms", "strip").hatHold(T0, F, 0, 5));
        all.add(start("inv/pad disconnect mid-hold (no release)", "licence").hat(T0, 0, F));
        all.add(start("inv/pad disconnect mid-hold then reconnect stick", "licence").hat(T0, 0, F).stickHold(T0 + 1500, 0, -F, 100));
        all.add(start("inv/RB during strip RIGHT walk", "strip").hatHold(T0, F, 0, 1500).tap(T0 + 450, KEY_R1, 60));
        all.add(start("inv/L1 then stick jitter at rest", "settings-column").stick(T0, 0, 0.9f).tap(T0 + 200, KEY_L1, 60).stick(T0 + 300, 0.02f, 0.88f).stick(T0 + 600, -0.01f, 0.91f).stick(T0 + 900, 0, 0));
        all.add(start("inv/Y restore then RIGHT held then A", "world-dirty-rows").tap(T0, KEY_Y, 80).hatHold(T0 + 300, -F, 0, 700).tap(T0 + 1200, KEYS_A, 80));
        all.add(start("inv/DPAD key DOWN held with HAT, RB mid", "controller-column").key(T0, KEY_DOWN, true).hat(T0, 0, F).tap(T0 + 600, KEY_R1, 60).hat(T0 + 1800, 0, 0).key(T0 + 1800, KEY_DOWN, false));
        all.add(start("inv/START save from rows during held RIGHT", "world-rows").hatHold(T0, -F, 0, 1200).tap(T0 + 600, KEY_START, 60));

        all.add(start("inv/RB during held LEFT on Traffic, one repeat inside the question, then A", "world-rows").hatHold(T0, -F, 0, 560).tap(T0 + 450, KEY_R1, 60).tap(T0 + 1200, KEYS_A, 80));
        all.add(start("inv/B,B during held LEFT on Traffic, one repeat inside the question, then A", "world-rows").hatHold(T0, -F, 0, 780).tap(T0 + 450, KEY_B, 60).tap(T0 + 700, KEY_B, 60).tap(T0 + 1500, KEYS_A, 80));
        all.add(start("inv/stick LEFT held on Traffic, RB, one repeat in the question, A", "world-rows").stickHold(T0, -F, 0, 560).tap(T0 + 450, KEY_R1, 60).tap(T0 + 1200, KEYS_A, 80));
        all.add(start("inv/RB during held RIGHT from Traffic 65 (dirty stays), one repeat, A", "world-rows").hatHold(T0 - 1500, -F, 0, 1040).hatHold(T0, F, 0, 560).tap(T0 + 450, KEY_R1, 60).tap(T0 + 1200, KEYS_A, 80));
        all.add(start("inv/pad with D-pad keys AND HAT: 10 ms DOWN tap in Licence", "licence").tap(T0, KEY_DOWN, 10).hatHold(T0, 0, F, 10));
        all.add(start("inv/pad with D-pad keys AND HAT: 120 ms DOWN tap, 300 ms frames", "licence").slow(T0 - 10, T0 + 1000, 300).tap(T0, KEY_DOWN, 120).hatHold(T0, 0, F, 120));
        all.add(start("inv/pad with D-pad keys AND HAT: 120 ms DOWN tap, 16 ms frames (control)", "licence").tap(T0, KEY_DOWN, 120).hatHold(T0, 0, F, 120));
        all.add(start("inv/pad with D-pad keys AND HAT: 120 ms RIGHT tap on Traffic, 300 ms frames", "world-rows").slow(T0 - 10, T0 + 1000, 300).tap(T0, KEY_RIGHT, 120).hatHold(T0, F, 0, 120));
        lane(all);
        // ---- random, fixed seed
        Random random = new Random(20261006L);
        String[] places = {"strip", "settings-column", "display-rows", "world-rows", "world-dirty", "licence", "framerate-rows", "controller-column"};
        int[] buttons = {KEYS_A, KEY_B, KEY_L1, KEY_R1, KEY_Y, KEY_START};
        for (int r = 0; r < 40; ++r) {
            Script s = start(String.format("rnd/%02d", r), places[random.nextInt(places.length)]);
            long t = T0;
            if (random.nextInt(3) == 0) s.slow(T0, T0 + 4000, 100 + random.nextInt(900));
            int steps = 4 + random.nextInt(10);
            for (int i = 0; i < steps; ++i) {
                int what = random.nextInt(6);
                if (what == 0) s.hat(t, random.nextInt(3) - 1, random.nextInt(3) - 1);
                else if (what == 1 || what == 2) s.stick(t, (random.nextFloat() * 2 - 1), (random.nextFloat() * 2 - 1));
                else if (what == 3) s.stick(t, 0, 0).hat(t, 0, 0);
                else s.tap(t, buttons[random.nextInt(buttons.length)], 20 + random.nextInt(300));
                t += 5 + random.nextInt(700);
            }
            s.stick(t, 0, 0).hat(t, 0, 0);
            all.add(s);
        }
        return all;
    }

    /** Scripts added by the lane after the checker's review (M2, M3, M4, minors 1 and 2): see MenuReplayExpected. */
    static void lane(List<Script> all) {
        // M2: questions opened under a held direction (Save changes? by RB / B; Restore original? by Y)
        all.add(start("lane/M2 RIGHT held on Traffic 65, RB, repeats in the question, A", "world-rows").hatHold(T0 - 1500, -F, 0, 1040)
                .hatHold(T0, F, 0, 1400).tap(T0 + 450, KEY_R1, 60).tap(T0 + 2000, KEYS_A, 80));
        all.add(start("lane/M2 LEFT held on Traffic, Y (restore question), B, LEFT still held", "world-rows").hatHold(T0, -F, 0, 2000)
                .tap(T0 + 450, KEY_Y, 60).tap(T0 + 1000, KEY_B, 60));
        all.add(start("lane/M2 question: LEFT released and pressed again moves once, A", "world-dirty").tap(T0, KEY_B, 80)
                .hatHold(T0 + 300, -F, 0, 900).hatHold(T0 + 1400, -F, 0, 80).tap(T0 + 1800, KEYS_A, 80));
        // M3: the three pad kinds, one tap each (keys only, HAT only, both), short and long, fast and slow frames
        all.add(start("lane/M3 keys only: 10 ms DOWN tap in Licence", "licence").tap(T0, KEY_DOWN, 10));
        all.add(start("lane/M3 HAT only: 10 ms DOWN tap in Licence", "licence").hatHold(T0, 0, F, 10));
        all.add(start("lane/M3 keys and HAT: two 30 ms taps in one 300 ms frame", "licence").slow(T0 - 10, T0 + 1000, 300)
                .tap(T0, KEY_DOWN, 30).hatHold(T0, 0, F, 30).tap(T0 + 100, KEY_DOWN, 30).hatHold(T0 + 100, 0, F, 30));
        all.add(start("lane/M3 keys and HAT: HAT batch first (unbuffered order), 10 ms", "licence").hatHold(T0 - 16, 0, F, 10)
                .tap(T0 + 5, KEY_DOWN, 10));
        all.add(start("lane/M3 keys and HAT: held 1000 ms", "licence").tap(T0, KEY_DOWN, 1000).hatHold(T0, 0, F, 1000));
        all.add(start("lane/M3 keys and HAT: RIGHT tap on Traffic, 300 ms frames", "world-rows").slow(T0 - 10, T0 + 1000, 300)
                .tap(T0, KEY_RIGHT, 40).hatHold(T0, F, 0, 40));
        // M4 and minor 1: sweeps and the band between the two zones
        all.add(start("lane/M4 sweep from up through the diagonal to the right on Scaler", "display-rows").stick(T0, 0, -0.6f)
                .stick(T0 + 8, 0.6f, -0.6f).stick(T0 + 16, F, -0.2f).stick(T0 + 400, 0, 0));
        all.add(start("lane/M4 sweep right then down without rest, then rest and down", "display-rows").stick(T0, F, 0)
                .stick(T0 + 100, 0.5f, 0.8f).stick(T0 + 200, 0, F).stick(T0 + 600, 0, 0).stickHold(T0 + 800, 0, F, 100));
        all.add(start("lane/minor1 (0.70, 0.55) held, then on to (0.9, 0.4)", "display-rows").stick(T0, 0.70f, 0.55f)
                .stick(T0 + 300, 0.9f, 0.4f).stick(T0 + 500, 0, 0));
        // minor 2: a held LEFT under the stick; a pad taken away mid-hold
        all.add(start("lane/minor2 HAT LEFT held on Traffic, stick DOWN, stick lets go, LEFT still held", "world-rows").hat(T0, -F, 0)
                .stickHold(T0 + 100, 0, F, 300).hat(T0 + 2500, 0, 0));
        // The pad is taken away (Android: onInputDeviceRemoved; it sends no release), then a pad comes back and its
        // stick is tapped: the new pad's HAT reads 0.
        all.add(start("lane/minor2 pad taken away mid-hold, then a stick tap", "licence").hat(T0, 0, F).removed(T0 + 300)
                .both(T0 + 1500, 0, 0, 0, -F).stick(T0 + 1600, 0, 0));
        // Round 3 (the second check): M1, M2 (a drifting stick), minor 6
        all.add(start("lane/r3 M1 keys and HAT: LEFT 5-200 and 250-330 ms in one 300 ms frame on Traffic", "world-rows")
                .slow(T0 - 10, T0 + 600, 300).key(T0 + 5, KEY_LEFT, true).hat(T0 + 5, -F, 0).key(T0 + 200, KEY_LEFT, false)
                .hat(T0 + 200, 0, 0).key(T0 + 250, KEY_LEFT, true).hat(T0 + 250, -F, 0).key(T0 + 330, KEY_LEFT, false).hat(T0 + 330, 0, 0));
        all.add(start("lane/r3 M2 stick resting at x 0.36: UP, back, RIGHT on Scaler", "display-rows").stick(T0, 0.36f, 0)
                .stick(T0 + 100, 0.36f, -F).stick(T0 + 200, 0.36f, 0).stick(T0 + 500, F, 0).stick(T0 + 600, 0, 0));
        all.add(start("lane/r3 minor6 stick LEFT held on Traffic, RB opens the question, D-pad LEFT", "world-rows")
                .stick(T0, -F, 0).tap(T0 + 450, KEY_R1, 60).hatHold(T0 + 900, -F, 0, 80).stick(T0 + 1500, 0, 0));
    }

    public static void main(String[] args) {
        boolean unbuffered = args.length > 0 && args[0].equals("unbuffered");
        for (Script s : scripts()) {
            Menu m;
            try {
                m = run(s, unbuffered);
                System.out.println("END " + s.name + " | " + m.state());
                System.out.println("CMD " + s.name + " | " + String.join(" ", m.commands));
            } catch (RuntimeException e) {
                System.out.println("END " + s.name + " | EXCEPTION " + e);
            }
        }
    }
}
