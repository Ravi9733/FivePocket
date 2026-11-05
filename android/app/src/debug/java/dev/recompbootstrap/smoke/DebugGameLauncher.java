package dev.recompbootstrap.smoke;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Intent;
import java.io.File;

/**
 * Starts the game activity from the launcher with the same component and the same four text extras a measurement
 * script passes to "am start" (design section 4.C). Found by LauncherScreen through its class name. The class keeps
 * its name although it is compiled into every build type since ship week: LauncherScreen looks it up by that name.
 *
 * In a build that carries its game code (ShipConfig), Play starts only what that app version installed and checked:
 * ShipPlay refuses while the first screen has not finished its work, while pinned game data is missing, and it
 * never accepts game code that is not the bundled one. In a developer build (no game code inside the APK) nothing
 * changes. Which of the two this build is, ShipPlay reads from the app's own APK file, whose path ShipApplication
 * remembers at process start (this class is given the files directory only). If that path is not known, Play is
 * refused: an unknown build is not treated as a developer build (ship_checks.sh checks that the merged manifest
 * names ShipApplication).
 */
public final class DebugGameLauncher implements GameLauncher {
    private volatile LaunchRequest.Prepared prepared;

    public DebugGameLauncher() {}

    @Override public String prepare(File filesDir, boolean acceptUnqualified) {
        prepared = null;
        ShipPlay.Result result = ShipPlay.prepare(ShipApplication.apkPath(), filesDir, acceptUnqualified);
        if (result.error != null) {
            return result.error;
        }
        prepared = result.request;
        return null;
    }

    @Override public void start(Activity activity) {
        LaunchRequest.Prepared request = prepared;
        prepared = null;   // a request id can be used once
        if (request == null) {
            return;
        }
        Intent intent = new Intent();
        intent.setComponent(new ComponentName(activity.getPackageName(), PrivateTraceActivity.class.getName()));
        for (String[] extra : request.extras()) {
            intent.putExtra(extra[0], extra[1]);
        }
        activity.startActivity(intent);
    }

    @Override public void startControllerCheck(Activity activity) {
        activity.startActivity(new Intent(activity, ControllerCheckActivity.class));
    }
}
