# Mobile: web, Android, iOS

Krunker v3.1.1 ran in mobile browsers with a dedicated touch UI, and its Android/iOS
apps were fullscreen web views around the web game. Krunker Box Office does the same: the
**web build is the mobile client**, and `platform/android` / `platform/ios` wrap it.

## Mobile web (the touch UI)

`client/src/ui/ui_mobile.c` ports v3.1.1's mobile code paths (unpacked client: 132.js
CONTROLS, 103.js UI, 134.js settings, 1.js `mobileControlPositions`, index.js):

| v3.1.1 | Krunker Box Office |
| --- | --- |
| `CONTROLS.checkMobile()`: Android/webOS/iPhone/iPad/iPod/BlackBerry/Windows Phone UA, or no `requestPointerLock` | same; plus iPadOS desktop-mode Safari (Macintosh UA with multi-touch) and `?mobile=1` / `?mobile=0` to force it |
| `UsePointerLock = ... && !isMobile`, `locklessChange()` | no pointer lock on phones; the menu never reopens for a missing lock |
| `"TAP TO PLAY"` | same text in `#instructions` |
| `#mobileUI` shown by `toggleControlUI` / `toggleGameUI` | shown while the game owns the input (alive or spectating, menu closed, no end screen) |
| `mobileControlPositions` layout pass | same percentages, `margin: auto` when left == right |
| nipplejs joystick (static, size 120, black) in `#mobileJoystick` | re-implemented in C: same DOM/inline styles (so main.css's `.back`/`.front` rules apply), same clamping, angles, 0.1 threshold and direction rules -> move keys; rest/fade transitions |
| `ae` table: jump, crouch, esc, shoot, aim+shoot (scope = shoot on release, others after `aimSpd + 50` ms, `nAuto` auto-fire), scope toggle, reload, swap; press `scale(1.1)` | same, through the keybind state the keyboard uses |
| touch drag on `#mobileUI` / aim-shoot / scope: `fakeMouse(2, dx, dy, 0, "mblInput")` | same deltas (screen px), `sensitivityXMbl`/`YMbl` x4, aim sensitivities while the right button is held, `invertYMbl` |
| `gradSpeedMbl` (default on): `speedLmt = distance / 50` | `ui_controls_set_speed_limit()` (sent as input[12] with the controller's Gradual Speed) |
| settings category "mobile" (`hide: !isMobile`) | rows and the Mobile header shown in Settings -> Controls on phones |
| `vibrationMbl` | kept as a setting; v3.1.1 only vibrates for controllers when *not* mobile, so it never fires on a phone |

Touches reach C through emscripten's `html5.h` touch callbacks registered on
`#mobileUI` (the element Krunker listened on); targets are hit-tested against the
button rects in paint order. nipplejs is not vendored: Krunker uses a tiny static
subset of it, and replicating that subset keeps the joystick logic next to the other
controls without a third-party script. Known v3.1.1 quirk not copied: nipplejs only
learns the joystick centre 100 ms after the first ever touch, so in v3.1.1 the very
first joystick touch does nothing.

Krunker Box Office additions (the Krunker *apps* did this, the v3.1.1 page did not): the TAP TO
PLAY gesture requests fullscreen and locks the screen to landscape where the browser
allows it (Android Chrome; iPhone Safari has no element fullscreen). Pages inside the
Krunker Box Office apps (user agent token `KrunkNativeApp`) skip it. v3.1.1 changes no
graphics settings on mobile, so neither does Krunker Box Office.

The page already carries v3.1.1's viewport meta (`user-scalable=0`), the `#mobileUI`
markup and the `img/mobile/` icons.

### Testing in desktop Chromium

DevTools device mode, or over CDP: `Emulation.setDeviceMetricsOverride` (`mobile: true`),
`Emulation.setUserAgentOverride` (an Android/iPhone UA), `Emulation.setTouchEmulationEnabled`,
then `Input.dispatchTouchEvent`. Or just open `/?offline=1&mobile=1` and use touch input.

## Native clients and touchscreens

GLFW has no touch API: on touchscreen Linux/Windows devices the OS turns touches into
mouse input, so a tap clicks (menu, shoot) and a drag moves the mouse (camera while
the pointer is locked). There is no on-screen joystick natively; use the web build on
touch-only devices.

## Android (`platform/android`)

A single-activity WebView app: fullscreen, immersive (system bars swipe in
transiently), `sensorLandscape`, screen kept on, back = Escape for the page. The web
client from `make web` (`dist/web`) is bundled into the APK and served from
`https://appassets.androidplatform.net/web/` by `WebViewAssetLoader` (a secure origin,
like the hosted game), WebSockets and cleartext LAN servers are allowed, links leave
the app, and controllers reach the page through the WebView's Gamepad API.

```sh
make web                    # dist/web
make android-only           # or `make android` (= web + APK); needs JDK 17+ and ANDROID_HOME
make android-install        # adb install -r .../app-debug.apk
```

Output: `platform/android/app/build/outputs/apk/debug/app-debug.apk`.

**Published APK** (`krunknative-android.apk` on the GitHub release `client-latest`, linked by the
menu's Download window, client/src/ui/download_ui.c): `assembleRelease` with
`-Pkrunknative.serverUrl=https://krunk.io/`, so the app is a thin fullscreen WebView over the live
site (no bundled client: `krunknative.bundleWeb` defaults to false with a server URL), always the
latest game, online play included. It is signed with the repository key:
`tools/release/android-keystore.sh` creates it once in `~/.config/kn-secrets/android/` (never
commit it: an update signed with another key does not install over the old app) and prints the
`gh secret set` commands for `ANDROID_KEYSTORE_BASE64`, `ANDROID_KEYSTORE_PASSWORD`,
`ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`; `make android-release` builds the same APK locally.
Gradle reads the key from `KN_ANDROID_KEYSTORE`, `KN_ANDROID_KEYSTORE_PASSWORD`,
`KN_ANDROID_KEY_ALIAS`, `KN_ANDROID_KEY_PASSWORD` (none: an unsigned release APK);
`-Pkrunknative.versionCode` / `versionName` (CI: commit time in minutes / git describe).

Toolchain: JDK 17 or 21, Android SDK with `platforms;android-35` and
`build-tools;35.0.0` (`sdkmanager`), Gradle 8.11.1 through the wrapper, AGP 8.7.3.

Configuration (Gradle properties: `-P...`, `ANDROID_ARGS="-P..."`, or
`platform/android/gradle.properties`):

| property | default | meaning |
| --- | --- | --- |
| `krunknative.webDir` | `../../dist/web` | web client to bundle |
| `krunknative.serverUrl` | empty | open this URL instead of the bundled client (a hosted game, like Krunker's apps) |
| `krunknative.wsUrl` | empty | bundled client: game server WebSocket (`?ws=`), e.g. `ws://192.168.1.10:8080/native` (a `make bridge` host); empty = embedded offline server (`?offline=1`) |
| `krunknative.assetBaseUrl` | empty | bundled client: fetch `/assets/*` missing from the APK from here, e.g. `http://192.168.1.10:8080/` |
| `krunknative.bundleAssets` | `tracked` | `none`, `tracked` (maps, img, shaders, skins.json) or `all` (the ~1.5 GB mod.zip content in `assets/`) |

At runtime `adb shell am start -n io.krunknative.app/.MainActivity -e url http://host:8080/`
opens any URL. Debug builds enable WebView remote debugging (`chrome://inspect`).

## iOS (`platform/ios`)

A UIKit app with a fullscreen landscape `WKWebView` (status bar and home indicator
hidden, edge gestures deferred, screen kept on). The bundled web client is served by a
small loopback HTTP server (`LocalServer.swift`, Network.framework) because the page
boots the wasm only from an http(s) origin; `KNServerURL`, `KNWebSocketURL` and
`KNAssetBaseURL` in `Info.plist` (set in `project.yml`) mirror the Android options.
Controllers work through WebKit's Gamepad API (`GCSupportsControllerUserInteraction`).

It cannot be built on Linux. On macOS with Xcode and [XcodeGen](https://github.com/yonaskolb/XcodeGen):

```sh
make web && make ios-prepare       # dist/web -> platform/ios/web
cd platform/ios && xcodegen generate
xcodebuild -project KrunkNative.xcodeproj -scheme KrunkNative -configuration Debug \
  -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' CODE_SIGNING_ALLOWED=NO build
```

For a device build open the generated project in Xcode and pick a signing team.

**Published .ipa** (`krunknative-ios-unsigned.ipa` on `client-latest`): an unsigned Release build
for devices with `KN_SERVER_URL=https://krunk.io/` (the `KNServerURL` build setting). iOS only
installs signed apps, so it is for sideloading tools (AltStore, Sideloadly), which sign it with
the player's Apple ID. The Download window sends iPhone / iPad players to Add to Home Screen
first: the web build is an installable web app (`platform/web`: `manifest.webmanifest`, icons and
the `<head>` links `pwa.py` adds to `dist/web/index.html`; `display: standalone`, landscape), so
Safari opens krunk.io from the Home Screen fullscreen, with no signing at all.

## CI

`.github/workflows/client.yml` publishes the mobile apps with the native packages on
`client-latest`: job `android` (the signed release APK above; skipped with a warning while the
`ANDROID_KEYSTORE_BASE64` secret is missing) and job `ios` (the unsigned .ipa).

`.github/workflows/build.yml`: `android` (Linux, downloads the `web` job's artifact,
`./gradlew assembleDebug`, uploads `KrunkNative-android-debug`) and `ios` (macOS,
XcodeGen + unsigned simulator `xcodebuild`, uploads `KrunkNative-ios-simulator`).
