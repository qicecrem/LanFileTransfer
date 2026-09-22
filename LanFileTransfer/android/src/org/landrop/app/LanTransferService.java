package org.landrop.app;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.app.Activity;
import android.app.DownloadManager;
import android.content.Context;
import android.content.ClipData;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.net.wifi.WifiManager;
import androidx.core.app.NotificationCompat;
import androidx.core.content.FileProvider;
import java.io.File;
import java.util.ArrayList;
import java.util.concurrent.atomic.AtomicInteger;

public final class LanTransferService extends Service {
    private static final String CHANNEL = "landrop_transfer";
    private static final AtomicInteger NEXT_NOTIFICATION = new AtomicInteger(46000);
    private static WifiManager.MulticastLock sharedMulticastLock;
    private static int multicastUsers;
    private PowerManager.WakeLock wakeLock;
    private boolean multicastAcquired;

    private static int notificationIcon(Context context) {
        int icon = context.getResources().getIdentifier("ic_stat_landrop", "drawable", context.getPackageName());
        return icon != 0 ? icon : android.R.drawable.stat_sys_upload;
    }

    public static void setEnabled(Context context, boolean enabled) {
        Intent intent = new Intent(context, LanTransferService.class);
        if (!enabled) { context.stopService(intent); return; }
        if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent);
        else context.startService(intent);
    }

    public static synchronized boolean acquireMulticast(Context context) {
        try {
            if (sharedMulticastLock == null) {
                WifiManager wifi = (WifiManager)context.getApplicationContext()
                        .getSystemService(WIFI_SERVICE);
                if (wifi == null) return false;
                sharedMulticastLock = wifi.createMulticastLock("LanDrop:Discovery");
                sharedMulticastLock.setReferenceCounted(false);
            }
            if (!sharedMulticastLock.isHeld()) sharedMulticastLock.acquire();
            ++multicastUsers;
            android.util.Log.i("LanDropService", "Multicast user acquired; users=" + multicastUsers);
            return true;
        } catch (RuntimeException exception) {
            sharedMulticastLock = null;
            multicastUsers = 0;
            android.util.Log.e("LanDropService", "Unable to acquire multicast lock", exception);
            return false;
        }
    }

    public static synchronized void releaseMulticast() {
        if (multicastUsers <= 0) return;
        --multicastUsers;
        if (multicastUsers == 0 && sharedMulticastLock != null) {
            try {
                if (sharedMulticastLock.isHeld()) sharedMulticastLock.release();
            } catch (RuntimeException exception) {
                android.util.Log.w("LanDropService", "Unable to release multicast lock", exception);
            }
            sharedMulticastLock = null;
        }
        android.util.Log.i("LanDropService", "Multicast user released; users=" + multicastUsers);
    }

    public static void requestRuntimePermissions(Activity activity) {
        if (Build.VERSION.SDK_INT < 23) return;
        ArrayList<String> missing = new ArrayList<>();
        String[] requested = Build.VERSION.SDK_INT >= 33
                ? new String[]{"android.permission.NEARBY_WIFI_DEVICES", "android.permission.POST_NOTIFICATIONS"}
                : new String[]{"android.permission.ACCESS_FINE_LOCATION"};
        for (String permission : requested)
            if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) missing.add(permission);
        if (!missing.isEmpty()) activity.requestPermissions(missing.toArray(new String[0]), 45454);
    }

    public static void requestMediaPermissions(Activity activity) {
        if (Build.VERSION.SDK_INT < 23) return;
        ArrayList<String> missing = new ArrayList<>();
        String[] requested = new String[]{"android.permission.CAMERA", "android.permission.RECORD_AUDIO"};
        for (String permission : requested)
            if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) missing.add(permission);
        if (!missing.isEmpty()) activity.requestPermissions(missing.toArray(new String[0]), 45456);
    }

    public static void openDownloads(Context context) {
        try {
            Intent intent = new Intent(DownloadManager.ACTION_VIEW_DOWNLOADS);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(intent);
        } catch (RuntimeException ignored) { }
    }

    public static void shareFile(Context context, String path) {
        try {
            File file = new File(path);
            Uri uri = FileProvider.getUriForFile(context,
                    context.getPackageName() + ".qtprovider", file);
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("application/zip");
            send.putExtra(Intent.EXTRA_STREAM, uri);
            send.setClipData(ClipData.newRawUri("LanDrop diagnostics", uri));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Intent chooser = Intent.createChooser(send, "Share LanDrop diagnostics");
            if (!(context instanceof Activity)) chooser.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            context.startActivity(chooser);
        } catch (RuntimeException exception) {
            android.util.Log.e("LanDropDiagnostics", "Unable to share diagnostic package", exception);
        }
    }

    public static void showNotification(Context context, String title, String message) {
        NotificationManager manager = (NotificationManager)context.getSystemService(Context.NOTIFICATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 26)
            manager.createNotificationChannel(new NotificationChannel(CHANNEL, "Nearby transfers", NotificationManager.IMPORTANCE_LOW));
        NotificationCompat.Builder builder = new NotificationCompat.Builder(context, CHANNEL);
        manager.notify(NEXT_NOTIFICATION.incrementAndGet(), builder.setContentTitle(title)
                .setContentText(message).setSmallIcon(notificationIcon(context)).setAutoCancel(true).build());
    }

    @Override public void onCreate() {
        super.onCreate();
        NotificationManager manager = getSystemService(NotificationManager.class);
        if (Build.VERSION.SDK_INT >= 26) {
            manager.createNotificationChannel(new NotificationChannel(CHANNEL, "Nearby transfers", NotificationManager.IMPORTANCE_LOW));
        }
        NotificationCompat.Builder builder = new NotificationCompat.Builder(this, CHANNEL);
        Notification notification = builder.setContentTitle("LanDrop").setContentText("Device discovery and transfers are active")
                .setSmallIcon(notificationIcon(this)).setOngoing(true).build();
        startForeground(45454, notification);
        PowerManager pm = (PowerManager)getSystemService(POWER_SERVICE);
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "LanDrop:Transfer");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();

        // Android filters multicast traffic by default on many devices. The
        // service and native scanner share one underlying lock through a
        // process-wide reference count.
        multicastAcquired = acquireMulticast(this);
    }
    @Override public int onStartCommand(Intent intent, int flags, int startId) { return START_STICKY; }
    @Override public void onTimeout(int startId, int foregroundServiceType) { stopSelf(); }
    @Override public void onDestroy() {
        if (multicastAcquired) releaseMulticast();
        multicastAcquired = false;
        if (wakeLock != null && wakeLock.isHeld()) wakeLock.release();
        wakeLock = null;
        super.onDestroy();
    }
    @Override public IBinder onBind(Intent intent) { return null; }
}
