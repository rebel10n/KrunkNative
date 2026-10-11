package io.krunknative.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.Window;
import android.view.WindowManager;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebResourceResponse;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import androidx.annotation.NonNull;
import androidx.core.view.WindowCompat;
import androidx.core.view.WindowInsetsCompat;
import androidx.core.view.WindowInsetsControllerCompat;
import androidx.webkit.WebViewAssetLoader;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * KrunkNative for Android: Krunker's mobile apps were a fullscreen landscape WebView
 * around the web game; this does the same for the KrunkNative web client.
 *
 * The bundled client (APK assets/web/, copied from `make web`'s dist/web by Gradle) is
 * served from https://appassets.androidplatform.net/web/ by a WebViewAssetLoader, so it
 * runs from a secure origin like the hosted game (wasm streaming, localStorage). Game
 * content the APK does not carry (/web/assets/*) is fetched from ASSET_BASE_URL when set.
 *
 * Which page opens: the `url` intent extra (adb shell am start -n io.krunknative.app/.MainActivity
 * -e url http://host:8080/), else BuildConfig.SERVER_URL, else the bundled client with
 * ?ws=WS_URL (a game server) or ?offline=1 (the embedded offline server).
 *
 * The page detects a phone by its user agent and shows Krunker's touch controls
 * (client/src/ui/ui_mobile.c); " KrunkNativeApp" in the user agent tells it the app
 * already runs fullscreen in landscape. Gamepads reach the page through the WebView's
 * Gamepad API (the WebView keeps focus, key/motion events are not consumed here).
 */
public class MainActivity extends Activity {
    private static final String TAG = "KrunkNative";
    private static final String APP_HOST = "appassets.androidplatform.net";
    private static final String BUNDLED_INDEX = "https://" + APP_HOST + "/web/index.html";

    private WebView webView;

    @SuppressLint("SetJavaScriptEnabled")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Window window = getWindow();
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        WindowCompat.setDecorFitsSystemWindows(window, false);

        WebView.setWebContentsDebuggingEnabled(BuildConfig.DEBUG);
        webView = new WebView(this);
        webView.setBackgroundColor(0xff000000);
        webView.setFocusable(true);
        webView.setFocusableInTouchMode(true);
        setContentView(webView);

        WebSettings s = webView.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);                  // localStorage: settings, binds
        s.setMediaPlaybackRequiresUserGesture(false);  // game audio
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setSupportZoom(false);
        s.setBuiltInZoomControls(false);
        s.setLoadWithOverviewMode(false);
        s.setUseWideViewPort(true);                    // honour the page's viewport meta
        s.setSupportMultipleWindows(false);
        // The bundled page is https; LAN game servers are usually ws:// / http://.
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_ALWAYS_ALLOW);
        s.setUserAgentString(s.getUserAgentString() + " KrunkNativeApp/" + BuildConfig.VERSION_NAME);

        final WebViewAssetLoader loader = new WebViewAssetLoader.Builder()
                .setDomain(APP_HOST)
                .addPathHandler("/web/", new WebPathHandler())
                .build();

        webView.setWebChromeClient(new WebChromeClient());
        webView.setWebViewClient(new WebViewClient() {
            @Override
            public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                return loader.shouldInterceptRequest(request.getUrl());
            }

            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri url = request.getUrl();
                if (isGameUrl(url)) return false;
                openExternal(url); // openURL(): Krunker's links (Discord, store pages, ...) leave the app
                return true;
            }
        });

        if (savedInstanceState != null) {
            webView.restoreState(savedInstanceState);
        } else {
            webView.loadUrl(startUrl());
        }
        webView.requestFocus();
        enterImmersive();
    }

    private String startUrl() {
        Intent intent = getIntent();
        String extra = intent != null ? intent.getStringExtra("url") : null;
        if (extra != null && !extra.isEmpty()) return extra;
        if (!BuildConfig.SERVER_URL.isEmpty()) return BuildConfig.SERVER_URL;
        if (!BuildConfig.WS_URL.isEmpty()) return BUNDLED_INDEX + "?ws=" + Uri.encode(BuildConfig.WS_URL);
        return BUNDLED_INDEX + "?offline=1";
    }

    private boolean isGameUrl(Uri url) {
        String host = url.getHost();
        if (host == null) return true;
        if (host.equals(APP_HOST)) return true;
        Uri start = Uri.parse(startUrl());
        return host.equals(start.getHost());
    }

    private void openExternal(Uri url) {
        try {
            startActivity(new Intent(Intent.ACTION_VIEW, url));
        } catch (ActivityNotFoundException e) {
            Log.w(TAG, "no app for " + url);
        }
    }

    // Immersive fullscreen: system bars hidden, swiped in transiently.
    private void enterImmersive() {
        WindowInsetsControllerCompat c = WindowCompat.getInsetsController(getWindow(), getWindow().getDecorView());
        c.setSystemBarsBehavior(WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        c.hide(WindowInsetsCompat.Type.systemBars());
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) enterImmersive();
    }

    @Override
    protected void onResume() {
        super.onResume();
        webView.onResume();
        webView.resumeTimers();
        enterImmersive();
    }

    @Override
    protected void onPause() {
        webView.onPause();
        webView.pauseTimers();
        super.onPause();
    }

    @Override
    protected void onSaveInstanceState(@NonNull Bundle out) {
        super.onSaveInstanceState(out);
        webView.saveState(out);
    }

    @Override
    protected void onDestroy() {
        webView.destroy();
        super.onDestroy();
    }

    // Back = Escape for the page (the menu / an open window), like the desktop key.
    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        long now = android.os.SystemClock.uptimeMillis();
        webView.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_ESCAPE, 0));
        webView.dispatchKeyEvent(new KeyEvent(now, now, KeyEvent.ACTION_UP, KeyEvent.KEYCODE_ESCAPE, 0));
    }

    /** Serves /web/** from the APK assets, falling back to ASSET_BASE_URL for game content. */
    private final class WebPathHandler implements WebViewAssetLoader.PathHandler {
        @Override
        public WebResourceResponse handle(@NonNull String path) {
            if (path.isEmpty() || path.endsWith("/")) path += "index.html";
            if (path.contains("..")) return notFound();
            String mime = mimeType(path);
            try {
                InputStream in = getAssets().open("web/" + path);
                return new WebResourceResponse(mime, null, 200, "OK", headers(), in);
            } catch (FileNotFoundException e) {
                // not bundled
            } catch (IOException e) {
                Log.w(TAG, "asset " + path + ": " + e);
            }
            if (path.startsWith("assets/") && !BuildConfig.ASSET_BASE_URL.isEmpty()) return remote(path, mime);
            return notFound();
        }

        private WebResourceResponse remote(String path, String mime) {
            String base = BuildConfig.ASSET_BASE_URL;
            try {
                URL url = new URL(base.endsWith("/") ? base + path : base + "/" + path);
                HttpURLConnection conn = (HttpURLConnection) url.openConnection();
                conn.setConnectTimeout(10000);
                conn.setReadTimeout(30000);
                int code = conn.getResponseCode();
                if (code != 200) {
                    conn.disconnect();
                    return notFound();
                }
                return new WebResourceResponse(mime, null, 200, "OK", headers(), conn.getInputStream());
            } catch (IOException e) {
                Log.w(TAG, "remote asset " + path + ": " + e);
                return notFound();
            }
        }

        private Map<String, String> headers() {
            Map<String, String> h = new HashMap<>();
            h.put("Access-Control-Allow-Origin", "*");
            h.put("Cache-Control", "no-cache");
            return h;
        }

        private WebResourceResponse notFound() {
            return new WebResourceResponse("text/plain", "utf-8", 404, "Not Found", headers(), null);
        }
    }

    private static String mimeType(String path) {
        int dot = path.lastIndexOf('.');
        String ext = dot < 0 ? "" : path.substring(dot + 1).toLowerCase(Locale.ROOT);
        switch (ext) {
            case "html": return "text/html";
            case "js": return "text/javascript";
            case "css": return "text/css";
            case "json": return "application/json";
            case "wasm": return "application/wasm";
            case "png": return "image/png";
            case "jpg": case "jpeg": return "image/jpeg";
            case "gif": return "image/gif";
            case "webp": return "image/webp";
            case "svg": return "image/svg+xml";
            case "ico": return "image/x-icon";
            case "ttf": return "font/ttf";
            case "otf": return "font/otf";
            case "woff": return "font/woff";
            case "woff2": return "font/woff2";
            case "mp3": return "audio/mpeg";
            case "ogg": return "audio/ogg";
            case "wav": return "audio/wav";
            case "txt": case "obj": case "mtl": case "glsl": case "vert": case "frag": return "text/plain";
            default: return "application/octet-stream";
        }
    }
}
