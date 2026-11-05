package dev.recompbootstrap.smoke;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;

/**
 * vm-pad-hold D7: "Save changes?" (B on a category name with an unsaved change, or L1 / R1 leaving the tab) opened
 * with Save highlighted, so a player backing out who pressed A wrote the files. Keep editing is highlighted first now.
 */
public final class SaveQuestionFocusTest {
    private static final String S = "SaveQuestionFocus";

    private static MenuModel dirtyModel() {
        MenuModel model = new MenuModel();
        model.loaded(Settings.defaults(), true, new ArrayList<String>(), new ArrayList<String>(),
                Capabilities.of(new HashSet<String>(Arrays.asList("diag-fps-overlay", "diag-v280-limit30", "diag-v280-limit60",
                        "diag-v409-scale-cubic", "diag-v409-scale-linear", "diag-v450-fsr1-stops", "diag-v450-scale-fsr1")), true, true));
        String label = SettingsSchema.spec(SettingsSchema.FPS_OVERLAY).label;
        int row = -1;
        for (int i = 0; i < model.rowCount(); ++i) if (label.equals(model.rowLabel(i))) row = i;
        for (int presses = 0; presses < model.rowCount() && model.focused() != row; ++presses) model.nav(NavInput.DOWN, false);
        model.nav(NavInput.RIGHT, false);
        MenuFixKit.check(model.dirty(), "one unsaved change");
        return model;
    }

    public static void main(String[] args) {
        MenuFixKit.requireAssertions();
        MenuFixKit.test(S, "B with an unsaved change, then A at once: the question closes, nothing is written or lost", () -> {
            MenuModel model = dirtyModel();
            model.nav(NavInput.BACK, false);
            MenuFixKit.eq(MenuModel.OVERLAY_SAVE_QUESTION, model.overlay(), "the question is open");
            MenuFixKit.eq("Keep editing", model.overlayButtonLabel(model.overlayFocused()), "the highlighted answer");
            model.nav(NavInput.CONFIRM, false);
            MenuFixKit.eq(MenuModel.OVERLAY_NONE, model.overlay(), "closed");
            MenuFixKit.eq(MenuModel.ACTION_NONE, model.pendingAction(), "no save, no leave");
            MenuFixKit.check(model.dirty(), "the change stands");
        });
        MenuFixKit.test(S, "Save and Discard stay one and two fresh steps to the left; repeats of a held LEFT move nothing", () -> {
            MenuModel model = dirtyModel();
            model.nav(NavInput.BACK, false);
            model.nav(NavInput.LEFT, false);
            MenuFixKit.eq("Discard", model.overlayButtonLabel(model.overlayFocused()), "one step left");
            model.nav(NavInput.LEFT, true);
            model.nav(NavInput.LEFT, true);
            MenuFixKit.eq("Discard", model.overlayButtonLabel(model.overlayFocused()), "repeats of the held LEFT (menu-input-fixes M2)");
            model.nav(NavInput.LEFT, false);
            MenuFixKit.eq("Save", model.overlayButtonLabel(model.overlayFocused()), "a fresh LEFT");
            model.nav(NavInput.CONFIRM, false);
            MenuFixKit.eq(MenuModel.ACTION_SAVE, model.pendingAction(), "A on Save saves, as before");
        });
        MenuFixKit.test(S, "control: Restore original? still opens on Cancel", () -> {
            MenuModel model = dirtyModel();
            model.nav(NavInput.ALT, false);
            MenuFixKit.eq("Cancel", model.overlayButtonLabel(model.overlayFocused()), "the highlighted answer");
        });
        MenuFixKit.finish(S);
    }
}
