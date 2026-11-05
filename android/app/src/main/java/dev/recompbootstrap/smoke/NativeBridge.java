package dev.recompbootstrap.smoke;

import android.view.Surface;

/**
 * Narrow Java/JNI boundary for the native runtime.
 *
 * Framework objects cross only explicit platform-adapter calls. Portable runtime
 * code remains independent of Android types behind the native renderer boundary.
 */
final class NativeBridge {
    static {
        System.loadLibrary("recomp_bootstrap");
    }

    private NativeBridge() {}

    static native boolean create(String dataDirectory, String cacheDirectory);
    static native boolean configureRgbaDiagnostic(byte[] rgba, boolean rejectReadback);
    static native void resume();
    static native void pause();
    static native void destroy();
    static native boolean surfaceCreated(Surface surface, int width, int height);
    static native boolean surfaceChanged(int width, int height);
    static native void surfaceDestroyed();
    static native String describe();
}
