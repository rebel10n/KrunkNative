// KrunkNative Android app: a fullscreen landscape WebView around the web client
// (Emscripten build, `make web` -> dist/web), the way Krunker's own mobile apps wrapped
// the web game. See platform/README.md.
plugins {
    id("com.android.application")
}

fun prop(name: String, def: String = ""): String = (project.findProperty(name) as String?)?.trim() ?: def
fun quoted(s: String): String = "\"" + s.replace("\\", "\\\\").replace("\"", "\\\"") + "\""

val repoRoot = rootProject.projectDir.resolve("../..").canonicalFile
val webDir = rootProject.projectDir.resolve(prop("krunknative.webDir", "../../dist/web")).canonicalFile
val bundleAssets = prop("krunknative.bundleAssets", "tracked")
val serverUrl = prop("krunknative.serverUrl")
// A hosted game (serverUrl, e.g. the release APK over https://krunk.io/) needs no bundled client.
val bundleWeb = prop("krunknative.bundleWeb", if (serverUrl.isEmpty()) "true" else "false") == "true"
val webAssetsOut = layout.buildDirectory.dir("generated/krunknative/assets")

// Release signing (CI: .github/workflows/client.yml, secrets ANDROID_KEYSTORE_BASE64 & co.;
// locally: tools/release/android-keystore.sh). Without KN_ANDROID_KEYSTORE the release APK is
// unsigned (app-release-unsigned.apk) and cannot be installed.
fun env(name: String): String = System.getenv(name)?.trim() ?: ""
val releaseKeystore = env("KN_ANDROID_KEYSTORE")

android {
    namespace = "io.krunknative.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.krunknative.app"
        minSdk = 24
        targetSdk = 35
        // CI: versionCode = commit time in minutes (monotonic, so every build installs over the
        // previous one), versionName = git describe
        versionCode = prop("krunknative.versionCode", "1").toInt()
        versionName = prop("krunknative.versionName", "7.1.4-kn")
        buildConfigField("String", "SERVER_URL", quoted(serverUrl))
        buildConfigField("String", "WS_URL", quoted(prop("krunknative.wsUrl")))
        buildConfigField("String", "ASSET_BASE_URL", quoted(prop("krunknative.assetBaseUrl")))
    }

    buildFeatures {
        buildConfig = true
    }

    signingConfigs {
        if (releaseKeystore.isNotEmpty()) {
            create("release") {
                storeFile = file(releaseKeystore)
                storePassword = env("KN_ANDROID_KEYSTORE_PASSWORD")
                keyAlias = env("KN_ANDROID_KEY_ALIAS")
                keyPassword = env("KN_ANDROID_KEY_PASSWORD").ifEmpty { env("KN_ANDROID_KEYSTORE_PASSWORD") }
            }
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
            if (releaseKeystore.isNotEmpty()) signingConfig = signingConfigs.getByName("release")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    // wasm/data must stay uncompressed-friendly and the generated web tree is an asset dir
    androidResources {
        noCompress += listOf("wasm", "data")
    }

    sourceSets {
        getByName("main") {
            assets.srcDir(webAssetsOut)
        }
    }
}

dependencies {
    implementation("androidx.webkit:webkit:1.12.1")
    implementation("androidx.core:core:1.13.1")
}

// Copy the assembled web client into the APK assets as web/ (without the game assets
// symlink), plus the selected part of <repo>/assets as web/assets/. A hosted-game build
// (bundleWeb false) carries none: the task empties the generated asset dir instead.
val copyWebClient = if (!bundleWeb) tasks.register<Delete>("copyWebClient") {
    description = "No bundled web client: the app opens $serverUrl"
    delete(webAssetsOut)
} else tasks.register<Sync>("copyWebClient") {
    description = "Bundles the KrunkNative web client (${webDir}) into the APK assets"
    into(webAssetsOut.map { it.dir("web") })
    from(webDir) {
        exclude("assets", "assets/**")
    }
    when (bundleAssets) {
        "none" -> {}
        "all" -> from(repoRoot.resolve("assets")) { into("assets") }
        else -> from(repoRoot.resolve("assets")) {
            into("assets")
            include("maps/**", "img/**", "shaders/**", "skins.json")
        }
    }
    doFirst {
        if (!webDir.resolve("client.wasm").isFile || !webDir.resolve("index.html").isFile) {
            throw GradleException("No web client in $webDir: run `make web` first (or pass -Pkrunknative.webDir=<dir>)")
        }
    }
}

tasks.named("preBuild") {
    dependsOn(copyWebClient)
}
