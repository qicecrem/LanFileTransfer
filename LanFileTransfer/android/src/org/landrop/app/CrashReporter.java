package org.landrop.app;

import android.content.Context;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;

/** Persists uncaught Java exceptions so they can be included in the next diagnostic package. */
public final class CrashReporter {
    private static final String TAG = "LanDropCrash";
    private static boolean installed;
    private static volatile File reportDirectory;

    private CrashReporter() { }

    public static synchronized void install(Context context) { install(context, null); }

    public static synchronized void install(Context context, String directoryPath) {
        if (directoryPath != null && !directoryPath.isEmpty())
            reportDirectory = new File(directoryPath);
        else if (reportDirectory == null)
            reportDirectory = new File(context.getFilesDir(), "diagnostics/crashes");
        if (installed) return;
        installed = true;
        final Context applicationContext = context.getApplicationContext();
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, throwable) -> {
            try {
                File directory = reportDirectory;
                if (directory.mkdirs() || directory.isDirectory()) {
                    File report = new File(directory, "java-crash-" + System.currentTimeMillis() + ".txt");
                    try (FileOutputStream stream = new FileOutputStream(report);
                         PrintWriter writer = new PrintWriter(
                                 new OutputStreamWriter(stream, StandardCharsets.UTF_8))) {
                        writer.println("thread=" + thread.getName());
                        writer.println("android=" + Build.VERSION.RELEASE + " sdk=" + Build.VERSION.SDK_INT);
                        writer.println("device=" + Build.MANUFACTURER + " " + Build.MODEL);
                        throwable.printStackTrace(writer);
                        writer.flush();
                        stream.getFD().sync();
                    }
                }
            } catch (Exception error) {
                Log.e(TAG, "Unable to persist Java crash", error);
            }
            if (previous != null) previous.uncaughtException(thread, throwable);
        });
    }
}
