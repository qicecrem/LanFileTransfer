package org.landrop.app;

import android.os.Build;
import android.util.Log;

/** Ensures Qt Multimedia's JNI_OnLoad runs before QtCamera2 is used. */
public final class QtMultimediaLoader {
    private static final String TAG = "LanDropMedia";
    private static boolean loaded;

    private QtMultimediaLoader() {}

    public static synchronized void ensureLoaded() {
        if (loaded)
            return;

        UnsatisfiedLinkError lastError = null;
        for (String abi : Build.SUPPORTED_ABIS) {
            final String library = "plugins_multimedia_ffmpegmediaplugin_" + abi;
            try {
                System.loadLibrary(library);
                loaded = true;
                Log.i(TAG, "Loaded Qt multimedia JNI library: " + library);
                return;
            } catch (UnsatisfiedLinkError error) {
                lastError = error;
                Log.w(TAG, "Unable to load " + library + ": " + error.getMessage());
            }
        }

        Log.e(TAG, "Qt multimedia JNI library could not be loaded", lastError);
    }
}
