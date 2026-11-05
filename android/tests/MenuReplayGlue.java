package dev.recompbootstrap.smoke;

import java.util.List;

/**
 * MenuReplay's glue for the lane's sources: what BootstrapActivity.LauncherHost and LauncherScreen do with input,
 * line for line (MenuInputGlueTest checks that those lines are in the app). Today's version is
 * local-analysis/agents/ship-week/menu-input-fixes/replay/today/MenuReplayGlue.java.
 */
final class MenuReplayGlue {
    private MenuReplayGlue() { }

    /** LauncherHost.motion: every sample of the batch with its time (NavInput.motion). */
    static void deliver(NavInput in, final List<float[]> batch) {
        in.motion(new NavInput.Motion() {
            @Override public int history() {
                return batch.size() - 1;
            }

            @Override public float value(int axis, int position) {
                float[] s = batch.get(position);
                switch (axis) {
                    case NavInput.AXIS_HAT_X: return s[1];
                    case NavInput.AXIS_HAT_Y: return s[2];
                    case NavInput.AXIS_X: return s[3];
                    case NavInput.AXIS_Y: return s[4];
                    default: return 0f;
                }
            }

            @Override public long time(int position) {
                return (long) batch.get(position)[0];
            }
        });
    }

    /** LauncherHost.key: the key event's own time. */
    static void key(NavInput in, int code, boolean down, long time) {
        in.key(code, down, 0, time);
    }

    /** LauncherHost.padRemoved. */
    static void removed(NavInput in) {
        in.reset();
    }

    /** LauncherScreen.carryOut, case TAB. */
    static void tabChanged(NavInput in, int answer, int command) {
        if (ShellNav.endsHold(answer, command)) in.endHold();
    }

    /** LauncherScreen.settingsLeft. */
    static void settingsLeft(NavInput in, int answer) {
        if (ShellNav.endsHold(answer, NavInput.NONE)) in.endHold();
    }

    /** LauncherScreen.onNav: a question (or a save) this command opened ends the hold. */
    static void afterCommand(NavInput in, boolean askedBefore, boolean askingNow) {
        if (!askedBefore && askingNow) in.endHold();
    }
}
