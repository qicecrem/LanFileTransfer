package org.landrop.app;

import android.app.DownloadManager;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.provider.DocumentsContract;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.URLConnection;

/** Android Storage Access Framework bridge used by the Qt transfer layer. */
public final class StorageBridge {
    private StorageBridge() { }

    public static boolean persistDirectory(Context context, String value) {
        try {
            Uri uri = Uri.parse(value);
            if (!"content".equals(uri.getScheme())) return false;
            int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
            context.getContentResolver().takePersistableUriPermission(uri, flags);
            return true;
        } catch (SecurityException | IllegalArgumentException exception) {
            return false;
        }
    }

    public static boolean copyToDirectory(Context context, String directoryValue,
                                          String sourcePath, String fileName) {
        Uri output = null;
        try {
            Uri tree = Uri.parse(directoryValue);
            ContentResolver resolver = context.getContentResolver();
            Uri parent = DocumentsContract.buildDocumentUriUsingTree(
                    tree, DocumentsContract.getTreeDocumentId(tree));
            String mime = URLConnection.guessContentTypeFromName(fileName);
            if (mime == null) mime = "application/octet-stream";
            output = DocumentsContract.createDocument(resolver, parent, mime, fileName);
            if (output == null) return false;
            try (InputStream input = new FileInputStream(new File(sourcePath));
                 OutputStream target = resolver.openOutputStream(output, "w")) {
                if (target == null) return false;
                byte[] buffer = new byte[1024 * 1024];
                int read;
                while ((read = input.read(buffer)) >= 0) {
                    if (read > 0) target.write(buffer, 0, read);
                }
                target.flush();
            }
            return true;
        } catch (Exception exception) {
            if (output != null) {
                try { DocumentsContract.deleteDocument(context.getContentResolver(), output); }
                catch (Exception ignored) { }
            }
            return false;
        }
    }

    public static void openDirectory(Context context, String value) {
        try {
            Uri tree = Uri.parse(value);
            Uri document = DocumentsContract.buildDocumentUriUsingTree(
                    tree, DocumentsContract.getTreeDocumentId(tree));
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(document, DocumentsContract.Document.MIME_TYPE_DIR);
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK |
                    Intent.FLAG_GRANT_READ_URI_PERMISSION |
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            context.startActivity(intent);
        } catch (RuntimeException ignored) {
            // Some vendor file managers do not advertise directory MIME support.
            // Open their regular downloads browser instead of showing the SAF
            // authorization picker again.
            try {
                Intent fallback = new Intent(DownloadManager.ACTION_VIEW_DOWNLOADS);
                fallback.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                context.startActivity(fallback);
            } catch (RuntimeException ignoredFallback) { }
        }
    }
}
