package com.employeesaccess.admin;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.MediaStore;
import android.text.InputType;
import android.webkit.CookieManager;
import android.webkit.JavascriptInterface;
import android.webkit.URLUtil;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * Shows the Employees Access admin page from the PC server.
 *
 * Android's WebView has no Web Bluetooth, so BleBridge provides it
 * natively; android-bridge.js (served with the page) turns that into
 * navigator.bluetooth for the unchanged web app.
 */
public class MainActivity extends Activity {

    static final String PREFS = "settings";
    static final String KEY_SERVER = "server_url";
    static final String DEFAULT_SERVER = "http://192.168.88.84:8080";

    private static final int PERMISSIONS_REQUEST = 1;

    private WebView web;
    private BleBridge ble;
    private boolean errorShown;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        web = new WebView(this);
        web.setBackgroundColor(0xFF0B1638);
        setContentView(web, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setMediaPlaybackRequiresUserGesture(true);
        s.setAllowFileAccess(false);
        s.setUserAgentString(s.getUserAgentString() + " EmployeesAccessApp/1.0");

        CookieManager.getInstance().setAcceptCookie(true);

        ble = new BleBridge(this, web);
        web.addJavascriptInterface(ble, "AndroidBle");
        web.addJavascriptInterface(new AppBridge(), "AndroidApp");

        // Default dialogs for confirm() / alert() (used when deleting employees)
        web.setWebChromeClient(new WebChromeClient());

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                // Stay inside the app for the server's own pages
                return !sameServer(request.getUrl().toString());
            }

            @Override
            public void onPageStarted(WebView view, String url, Bitmap favicon) {
                errorShown = false;
            }

            @Override
            public void onReceivedError(WebView view, WebResourceRequest request, WebResourceError error) {
                if (request.isForMainFrame() && !errorShown) {
                    errorShown = true;
                    showConnectionError(String.valueOf(error.getDescription()));
                }
            }
        });

        web.setDownloadListener((url, userAgent, contentDisposition, mimeType, length) ->
                downloadFile(url, userAgent, contentDisposition, mimeType));

        requestBluetoothPermissions();

        if (savedInstanceState != null) {
            web.restoreState(savedInstanceState);
        } else if (!prefs().contains(KEY_SERVER)) {
            askServer(true);
        } else {
            web.loadUrl(server());
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        web.saveState(outState);
    }

    @Override
    public void onBackPressed() {
        if (web.canGoBack()) {
            web.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onDestroy() {
        ble.shutdown();
        web.destroy();
        super.onDestroy();
    }

    /* ---------------- Server address ---------------- */

    SharedPreferences prefs() {
        return getSharedPreferences(PREFS, MODE_PRIVATE);
    }

    String server() {
        return prefs().getString(KEY_SERVER, DEFAULT_SERVER);
    }

    private boolean sameServer(String url) {
        Uri a = Uri.parse(url);
        Uri b = Uri.parse(server());
        return a.getHost() != null && a.getHost().equals(b.getHost()) && a.getPort() == b.getPort();
    }

    void askServer(boolean firstRun) {
        EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        input.setText(server());
        input.setSelection(input.getText().length());

        FrameLayout box = new FrameLayout(this);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);
        box.addView(input);

        AlertDialog.Builder dialog = new AlertDialog.Builder(this)
                .setTitle("Server address")
                .setMessage("The address of the PC running serve.py, on the same Wi-Fi.")
                .setView(box)
                .setCancelable(!firstRun)
                .setPositiveButton("Connect", (d, w) -> {
                    String url = input.getText().toString().trim();
                    if (!url.startsWith("http://") && !url.startsWith("https://")) {
                        url = "http://" + url;
                    }
                    while (url.endsWith("/")) {
                        url = url.substring(0, url.length() - 1);
                    }
                    prefs().edit().putString(KEY_SERVER, url).apply();
                    web.loadUrl(url);
                });

        if (!firstRun) {
            dialog.setNegativeButton("Cancel", null);
        }

        dialog.show();
    }

    private void showConnectionError(String reason) {
        web.loadData("<html><body style='background:#0b1638'></body></html>", "text/html", "utf-8");

        new AlertDialog.Builder(this)
                .setTitle("Can't reach the server")
                .setMessage("Couldn't open " + server() + "\n\n(" + reason + ")\n\n"
                        + "Check that the PC is on, serve.py is running, and this phone "
                        + "is on the same Wi-Fi.")
                .setCancelable(false)
                .setPositiveButton("Retry", (d, w) -> web.loadUrl(server()))
                .setNeutralButton("Change address", (d, w) -> askServer(false))
                .show();
    }

    /* ---------------- Permissions ---------------- */

    private void requestBluetoothPermissions() {
        List<String> needed = new ArrayList<>();

        if (Build.VERSION.SDK_INT >= 31) {
            needed.add(Manifest.permission.BLUETOOTH_SCAN);
            needed.add(Manifest.permission.BLUETOOTH_CONNECT);
        } else {
            needed.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }

        if (Build.VERSION.SDK_INT <= 28) {
            needed.add(Manifest.permission.WRITE_EXTERNAL_STORAGE);
        }

        List<String> missing = new ArrayList<>();
        for (String p : needed) {
            if (checkSelfPermission(p) != PackageManager.PERMISSION_GRANTED) {
                missing.add(p);
            }
        }

        if (!missing.isEmpty()) {
            requestPermissions(missing.toArray(new String[0]), PERMISSIONS_REQUEST);
        }
    }

    boolean hasBluetoothPermissions() {
        if (Build.VERSION.SDK_INT >= 31) {
            return checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED
                    && checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED;
        }
        return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    void askBluetoothPermissions() {
        requestBluetoothPermissions();
    }

    /* ---------------- Downloads ---------------- */

    // Links such as the data sheet's "Download CSV" (needs the login cookie)
    private void downloadFile(String url, String userAgent, String contentDisposition, String mimeType) {
        try {
            String name = URLUtil.guessFileName(url, contentDisposition, mimeType);
            DownloadManager.Request request = new DownloadManager.Request(Uri.parse(url))
                    .addRequestHeader("Cookie", CookieManager.getInstance().getCookie(url))
                    .addRequestHeader("User-Agent", userAgent)
                    .setMimeType(mimeType)
                    .setTitle(name)
                    .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                    .setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name);

            ((DownloadManager) getSystemService(Context.DOWNLOAD_SERVICE)).enqueue(request);
            Toast.makeText(this, "Downloading " + name, Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            Toast.makeText(this, "Download failed: " + e.getMessage(), Toast.LENGTH_LONG).show();
        }
    }

    // Files built inside the page (the Logs page's CSV export)
    boolean saveToDownloads(String name, String mimeType, String text) {
        byte[] data = text.getBytes(StandardCharsets.UTF_8);

        try {
            if (Build.VERSION.SDK_INT >= 29) {
                ContentValues values = new ContentValues();
                values.put(MediaStore.Downloads.DISPLAY_NAME, name);
                values.put(MediaStore.Downloads.MIME_TYPE, mimeType);
                Uri uri = getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                if (uri == null) {
                    return false;
                }
                try (OutputStream out = getContentResolver().openOutputStream(uri)) {
                    out.write(data);
                }
            } else {
                File dir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
                dir.mkdirs();
                try (OutputStream out = new FileOutputStream(new File(dir, name))) {
                    out.write(data);
                }
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    /* ---------------- window.AndroidApp ---------------- */

    private class AppBridge {
        @JavascriptInterface
        public boolean saveFile(String name, String mimeType, String text) {
            return saveToDownloads(name.replaceAll("[^A-Za-z0-9._-]", "_"), mimeType, text);
        }

        @JavascriptInterface
        public void changeServer() {
            runOnUiThread(() -> askServer(false));
        }

        @JavascriptInterface
        public String version() {
            return "1.0";
        }
    }
}
