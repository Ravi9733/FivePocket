package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.Context;
import android.os.Build;

/**
 * Asks Android how the game's process ended. A class of its own because the question exists from Android 11
 * (API 30) on: on an older Android this class is never loaded, so the launcher screen itself names nothing that
 * such a device lacks. What each of Android's reasons means for the launcher is LaunchOutcome.exitFor's.
 */
final class LaunchExit {
    private LaunchExit() {}

    /**
     * How Android says the game's process ended since a start made at sinceWallMs (wall-clock time), and in
     * ranMs[0] how long after that start; UNKNOWN_OR_ORDINARY (ranMs[0] untouched) when Android has no record, or
     * on Android 10 and older, which cannot be asked. Android lists the newest ending first. Never throws.
     */
    static LaunchOutcome.Exit since(Activity activity, long sinceWallMs, long[] ranMs) {
        if (Build.VERSION.SDK_INT < 30) {
            return LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY;
        }
        try {
            return Api30.since(activity, sinceWallMs, ranMs);
        } catch (RuntimeException | LinkageError unavailable) {
            return LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY;   // nothing is known then
        }
    }

    @android.annotation.TargetApi(30)
    private static final class Api30 {
        static LaunchOutcome.Exit since(Activity activity, long sinceWallMs, long[] ranMs) {
            ActivityManager manager = (ActivityManager) activity.getSystemService(Context.ACTIVITY_SERVICE);
            if (manager == null) {
                return LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY;
            }
            String process = activity.getPackageName() + ScreenStack.GAME_PROCESS_SUFFIX;
            for (ApplicationExitInfo info : manager.getHistoricalProcessExitReasons(activity.getPackageName(), 0, 8)) {
                if (process.equals(info.getProcessName()) && info.getTimestamp() >= sinceWallMs) {
                    ranMs[0] = info.getTimestamp() - sinceWallMs;
                    return LaunchOutcome.exitFor(info.getReason());
                }
            }
            return LaunchOutcome.Exit.UNKNOWN_OR_ORDINARY;
        }
    }
}
