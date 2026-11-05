package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;

/**
 * vm-pad-hold D2 (major): the left stick at an exact diagonal changed a Settings value (Display > Scaler, FSR 1 ->
 * Snapdragon GSR 1) because a tie went sideways. A tie and anything near a diagonal now go vertical, as the D-pad's
 * corner does; sideways needs |x| > NavInput.STICK_SIDEWAYS (1.4) * |y|. Clean presses along an axis are unchanged.
 */
public final class StickDiagonalTest {
    private static final String S = "StickDiagonal";

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "the exact diagonals of the stick go vertical, never sideways", () -> {
            float[][] corners = {{1f, 1f}, {-1f, 1f}, {1f, -1f}, {-1f, -1f}, {0.7071f, 0.7071f}};
            String[] expected = {"DOWN@0", "DOWN@0", "UP@0", "UP@0", "DOWN@0"};
            for (int i = 0; i < corners.length; ++i) {
                MenuFixKit.Rig rig = new MenuFixKit.Rig();
                rig.input.axes(0f, 0f, corners[i][0], corners[i][1]);
                MenuFixKit.eq(expected[i], rig.take(), "stick " + Arrays.toString(corners[i]));
            }
        });
        MenuFixKit.test(S, "the sideways lead: 36 degrees from the horizontal is vertical, 34 degrees is sideways", () -> {
            MenuFixKit.Rig near = new MenuFixKit.Rig();
            near.input.axes(0f, 0f, (float) Math.cos(Math.toRadians(36)), (float) Math.sin(Math.toRadians(36)));
            MenuFixKit.eq("DOWN@0", near.take(), "36 degrees (|x| = 1.38 |y|): vertical");
            MenuFixKit.Rig clear = new MenuFixKit.Rig();
            clear.input.axes(0f, 0f, -(float) Math.cos(Math.toRadians(34)), (float) Math.sin(Math.toRadians(34)));
            MenuFixKit.eq("LEFT@0", clear.take(), "34 degrees (|x| = 1.48 |y|): sideways");
        });
        MenuFixKit.test(S, "control: clean pushes along an axis are what they were (thresholds 0.55 / 0.35 unchanged)", () -> {
            MenuFixKit.Rig rig = new MenuFixKit.Rig();
            rig.input.axes(0f, 0f, 0.55f, 0f);
            MenuFixKit.eq("", rig.take(), "0.55: nothing");
            rig.input.axes(0f, 0f, 0.56f, 0.05f);
            MenuFixKit.eq("RIGHT@0", rig.take(), "0.56 with a little y");
            rig.input.axes(0f, 0f, 0.34f, 0f);
            rig.input.axes(0f, 0f, -1f, 0f);
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.input.axes(0f, 0f, 0f, -1f);
            rig.input.axes(0f, 0f, 0f, 0f);
            rig.input.axes(0f, 0f, 0f, 1f);
            MenuFixKit.eq("LEFT@0 UP@0 DOWN@0", rig.take(), "full pushes left, up, down");
        });
        MenuFixKit.test(S, "Settings > Display, Scaler row: stick (1.0, 1.0) for 275 ms leaves the value as it was", () -> {
            final MenuModel model = new MenuModel();
            model.loaded(Settings.defaults(), true, new ArrayList<String>(), new ArrayList<String>(),
                    Capabilities.of(new HashSet<String>(Arrays.asList("diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60",
                            "diag-v409-scale-cubic", "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1",
                            "diag-v460-scale-gsr1", "scaler.txt")), true, true));
            int scaler = -1;
            String label = SettingsSchema.spec(SettingsSchema.SCALER).label;
            for (int i = 0; i < model.rowCount(); ++i) if (label.equals(model.rowLabel(i))) scaler = i;
            MenuFixKit.check(scaler >= 0, "the Scaler row exists");
            for (int presses = 0; presses < model.rowCount() && model.focused() != scaler; ++presses) model.nav(NavInput.DOWN, false);
            MenuFixKit.eq(scaler, model.focused(), "on Scaler");
            String before = model.rowValue(scaler);
            MenuFixKit.Rig rig = new MenuFixKit.Rig() {
                @Override void consume(int command, boolean repeat) {
                    model.nav(command, repeat);
                }
            };
            rig.input.axes(0f, 0f, 1f, 1f);
            rig.at(275);
            rig.input.axes(0f, 0f, 0f, 0f);
            MenuFixKit.eq(before, model.rowValue(scaler), "the Scaler value");
            MenuFixKit.check(!model.dirty(), "nothing unsaved");
            MenuFixKit.eq("DOWN@0", rig.take(), "what the stick sent");
        });
        MenuFixKit.finish(S);
    }
}
