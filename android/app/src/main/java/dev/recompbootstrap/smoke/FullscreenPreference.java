package dev.recompbootstrap.smoke;

import android.content.Context;
import android.content.SharedPreferences;

/** Persistent app-level display preference; deliberately separate from game runtime settings and saves. */
final class FullscreenPreference {
    private static final String FILE = "display_preferences";
    private static final String KEY_FULLSCREEN = "fullscreen";
    private FullscreenPreference() {}

    static boolean enabled(Context context) {
        return preferences(context).getBoolean(KEY_FULLSCREEN, true);
    }

    static boolean setEnabled(Context context, boolean enabled) {
        return preferences(context).edit().putBoolean(KEY_FULLSCREEN, enabled).commit();
    }

    private static SharedPreferences preferences(Context context) {
        return context.getSharedPreferences(FILE, Context.MODE_PRIVATE);
    }
}
