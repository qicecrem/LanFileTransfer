package org.landrop.app;

import android.content.Intent;
import android.content.Context;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.qtproject.qt.android.bindings.QtActivity;

public final class LanTransferActivity extends QtActivity {
    private static final int RECEIVE_DIRECTORY_REQUEST = 45455;
    private static final String TAG = "LanDropNetwork";
    private final Handler networkHandler = new Handler(Looper.getMainLooper());
    private ConnectivityManager connectivityManager;
    private ConnectivityManager.NetworkCallback networkCallback;

    private native void nativeReceiveDirectorySelected(String uri);
    private native void nativeNetworkChanged();

    private final Runnable notifyNetworkChanged = new Runnable() {
        @Override public void run() { nativeNetworkChanged(); }
    };

    private void scheduleNetworkChanged() {
        networkHandler.removeCallbacks(notifyNetworkChanged);
        networkHandler.postDelayed(notifyNetworkChanged, 750);
    }

    @Override
    public void onCreate(Bundle state) {
        super.onCreate(state);
        CrashReporter.install(this);
        connectivityManager = (ConnectivityManager)getSystemService(Context.CONNECTIVITY_SERVICE);
        if (connectivityManager == null) return;
        networkCallback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) { scheduleNetworkChanged(); }
            @Override public void onLost(Network network) { scheduleNetworkChanged(); }
            @Override public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) {
                scheduleNetworkChanged();
            }
            @Override public void onLinkPropertiesChanged(Network network, LinkProperties properties) {
                scheduleNetworkChanged();
            }
        };
        try {
            connectivityManager.registerDefaultNetworkCallback(networkCallback);
        } catch (RuntimeException exception) {
            Log.w(TAG, "Unable to register network callback", exception);
            networkCallback = null;
        }
    }

    @Override
    public void onResume() {
        super.onResume();
        scheduleNetworkChanged();
    }

    @Override
    public void onDestroy() {
        networkHandler.removeCallbacks(notifyNetworkChanged);
        if (connectivityManager != null && networkCallback != null) {
            try { connectivityManager.unregisterNetworkCallback(networkCallback); }
            catch (RuntimeException ignored) { }
        }
        networkCallback = null;
        super.onDestroy();
    }

    public void requestReceiveDirectory() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION |
                Intent.FLAG_GRANT_PREFIX_URI_PERMISSION);
        startActivityForResult(intent, RECEIVE_DIRECTORY_REQUEST);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != RECEIVE_DIRECTORY_REQUEST || resultCode != RESULT_OK || data == null)
            return;
        Uri uri = data.getData();
        if (uri == null) return;
        int flags = data.getFlags() & (Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        try { getContentResolver().takePersistableUriPermission(uri, flags); }
        catch (SecurityException ignored) { return; }
        nativeReceiveDirectorySelected(uri.toString());
    }
}
