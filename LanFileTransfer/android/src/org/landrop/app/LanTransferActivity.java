package org.landrop.app;

import android.content.Intent;
import android.content.Context;
import android.content.ClipData;
import android.net.ConnectivityManager;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.Log;

import org.qtproject.qt.android.bindings.QtActivity;

import java.util.ArrayList;

public final class LanTransferActivity extends QtActivity {
    private static final int RECEIVE_DIRECTORY_REQUEST = 45455;
    private static final int SEND_FILES_REQUEST = 45457;
    private static final String TAG = "LanDropNetwork";
    private final Handler networkHandler = new Handler(Looper.getMainLooper());
    private ConnectivityManager connectivityManager;
    private ConnectivityManager.NetworkCallback networkCallback;

    private native void nativeReceiveDirectorySelected(String uri);
    private native void nativeFilesSelected(String[] uris);
    private native void nativeNetworkChanged();
    private native void nativeMediaPermissionsChanged();

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
        try { nativeMediaPermissionsChanged(); }
        catch (UnsatisfiedLinkError ignored) { }
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

    public void requestFiles() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, SEND_FILES_REQUEST);
    }

    public void requestMediaPermissions() {
        LanTransferService.requestMediaPermissions(this);
    }

    public void openApplicationSettings() {
        Intent intent = new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                Uri.fromParts("package", getPackageName(), null));
        startActivity(intent);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode == 45456) {
            try { nativeMediaPermissionsChanged(); }
            catch (UnsatisfiedLinkError ignored) { }
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == SEND_FILES_REQUEST) {
            if (resultCode != RESULT_OK || data == null) return;
            ArrayList<String> selected = new ArrayList<>();
            ClipData clip = data.getClipData();
            if (clip != null) {
                for (int index = 0; index < clip.getItemCount(); ++index)
                    appendSelectedFile(data, clip.getItemAt(index).getUri(), selected);
            } else {
                appendSelectedFile(data, data.getData(), selected);
            }
            if (!selected.isEmpty()) nativeFilesSelected(selected.toArray(new String[0]));
            return;
        }
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

    private void appendSelectedFile(Intent result, Uri uri, ArrayList<String> selected) {
        if (uri == null) return;
        int flags = result.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION;
        try { getContentResolver().takePersistableUriPermission(uri, flags); }
        catch (SecurityException | IllegalArgumentException exception) {
            Log.w(TAG, "Document provider did not grant persistent access: " + uri, exception);
        }
        selected.add(uri.toString());
    }
}
