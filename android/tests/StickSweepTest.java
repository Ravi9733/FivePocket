package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;

/**
 * menu-input-fixes M4 and minor 1. One stick push gives at most one direction: once it gave one, a direction of the
 * other axis needs the stick back at rest (both axes inside 0.35). A push that sweeps past the diagonal keeps its
 * first command: on Scaler the sweep (0.3,0.3) (0.6,0.6) (0.9,0.4) (1,0) moves the highlight down once and changes
 * no value (today it changed Scaler; the first round changed FSR Sharpness). Minor 1, the stated rule: a push between
 * the sideways zone (|x| > 1.4 |y|) and the vertical one (|y| > 0.55) gives nothing until it enters one of them.
 */
public final class StickSweepTest {
    private static final String S = "StickSweep";

    private static MenuModel onScaler() {
        MenuModel model = new MenuModel();
        model.loaded(Settings.defaults(), true, new ArrayList<String>(), new ArrayList<String>(),
                Capabilities.of(new HashSet<String>(Arrays.asList("diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60",
                        "diag-v409-scale-cubic", "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1",
                        "diag-v460-scale-gsr1", "scaler.txt")), true, true));
        String label = SettingsSchema.spec(SettingsSchema.SCALER).label;
        for (int presses = 0; presses < model.rowCount() && !label.equals(model.rowLabel(model.focused())); ++presses) {
            model.nav(NavInput.DOWN, false);
        }
        return model;
    }

    private static MenuFixKit.Rig into(final MenuModel model) {
        return new MenuFixKit.Rig() {
            @Override void consume(int command, boolean repeat) {
                model.nav(command, repeat);
            }
        };
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "the checker's sweep on Scaler, 8 ms apart: one DOWN, no value changes", () -> {
            MenuModel model = onScaler();
            MenuFixKit.Rig rig = into(model);
            float[][] sweep = {{0.3f, 0.3f}, {0.6f, 0.6f}, {0.9f, 0.4f}, {1f, 0f}};
            for (float[] s : sweep) {
                rig.input.axes(0f, 0f, s[0], s[1]);
                rig.at(rig.timer.now + 8);
            }
            rig.at(400);
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.at(3000);
            MenuFixKit.eq("DOWN@8", rig.take(), "commands");
            MenuFixKit.check(!model.dirty(), "no value changed");
        });
        MenuFixKit.test(S, "the same sweep read from one slow frame's batch: the same single DOWN", () -> {
            MenuModel model = onScaler();
            MenuFixKit.Rig rig = into(model);
            final float[][] sweep = {{0.3f, 0.3f}, {0.6f, 0.6f}, {0.9f, 0.4f}, {1f, 0f}};
            rig.at(250);
            rig.input.motion(new NavInput.Motion() {
                @Override public int history() {
                    return sweep.length - 1;
                }

                @Override public float value(int axis, int position) {
                    return axis == NavInput.AXIS_X ? sweep[position][0] : axis == NavInput.AXIS_Y ? sweep[position][1] : 0f;
                }

                @Override public long time(int position) {
                    return position * 8L;
                }
            });
            rig.at(400);
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.at(3000);
            MenuFixKit.eq("DOWN@250", rig.take(), "commands");
            MenuFixKit.check(!model.dirty(), "no value changed");
        });
        MenuFixKit.test(S, "a push rolled from RIGHT to DOWN gives nothing; its own axis's other side works; after rest DOWN works", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 0f, 1f, 0f);
            rig.input.axes(0f, 0f, 0.5f, 0.8f);
            rig.input.axes(0f, 0f, 0f, 1f);
            rig.at(2000);
            MenuFixKit.eq("RIGHT@0", rig.take(), "rolled on to DOWN without rest");
            rig.input.axes(0f, 0f, -1f, 0f);
            // The same axis as the push's first command: allowed without rest (a flick straight across the centre
            // that the sampling missed looks the same).
            MenuFixKit.eq("LEFT@2000", rig.take(), "on round to LEFT, the push's own axis: LEFT");
            rig.input.axes(0f, 0f, 0.1f, 0.2f);
            rig.input.axes(0f, 0f, 0f, 1f);
            MenuFixKit.eq("DOWN@2000", rig.take(), "after rest: DOWN");
            rig.input.axes(0f, 0f, 0f, -1f);
            MenuFixKit.eq("UP@2000", rig.take(), "straight across to UP in one sample (the same axis): UP");
        });
        MenuFixKit.test(S, "minor 1, stated: (0.70, 0.55) and (0.613, 0.514) give nothing; on into the sideways zone, sideways", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 0f, 0.70f, 0.55f);
            rig.input.axes(0f, 0f, 0.613f, 0.514f);
            rig.at(1000);
            MenuFixKit.eq("", rig.take(), "the band between the zones");
            rig.input.axes(0f, 0f, 0.9f, 0.4f);
            MenuFixKit.eq("RIGHT@1000", rig.take(), "|x| = 2.25 |y|: sideways");
            MenuFixKit.Rig down = new MenuFixKit.Rig();
            down.input.axes(0f, 0f, 0.70f, 0.55f);
            down.input.axes(0f, 0f, 0.70f, 0.60f);
            MenuFixKit.eq("DOWN@0", down.take(), "on into the vertical zone (|y| 0.60 > 0.55, no sideways lead): vertical");
        });
        MenuFixKit.finish(S);
    }
}
