package dev.recompbootstrap.smoke;

import android.app.Application;
import java.io.IOException;
import java.util.zip.ZipFile;

/**
 * Remembers where the app's own APK file is, for code that has no Context at hand (DebugGameLauncher.prepare is
 * given the files directory only), and whether that APK carries its game code. Set when a process of the app
 * starts. It also gives ShipVerify (pure Java) Android's log to write its VERIFY_ lines to; nothing else happens here.
 */
public final class ShipApplication extends Application {
    private static volatile String apkPath;
    private static volatile Boolean carriesGameCode;

    @Override public void onCreate() {
        super.onCreate();
        apkPath = getApplicationInfo().sourceDir;
        // One log line per whole read of the game code file and the shader file, with its time (ShipVerify).
        ShipVerify.setSink(new ShipVerify.Sink() {
            @Override public void line(String line) {
                android.util.Log.i(ShipLog.TAG, line);
            }
        });
    }

    /** The APK the app runs from, or null before the first process start has reached onCreate. */
    static String apkPath() {
        return apkPath;
    }

    /**
     * True when this APK has the record of bundled game code (the asset ship/config.json): a ship build. False in a
     * developer build, and while the APK path is not known. Looked up once per process; reads the APK's list of
     * entries only.
     */
    static boolean publicApp() { return PublicGameCode.isPublic(apkPath); }

    static boolean playerApp() { return publicApp() || carriesGameCode(); }

    static boolean carriesGameCode() {
        Boolean known = carriesGameCode;
        if (known != null) return known.booleanValue();
        String path = apkPath;
        if (path == null) return false;
        boolean found;
        try (ZipFile zip = new ZipFile(path)) {
            found = zip.getEntry("assets/" + ShipConfig.ASSET) != null;
        } catch (IOException | RuntimeException unreadable) {
            return false;
        }
        carriesGameCode = Boolean.valueOf(found);
        return found;
    }
}
