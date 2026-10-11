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
val webAssetsOut = layout.buildDirectory.dir("generated/krunknative/assets")

android {
    namespace = "io.krunknative.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "io.krunknative.app"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "3.1.1-kn"
        buildConfigField("String", "SERVER_URL", quoted(prop("krunknative.serverUrl")))
        buildConfigField("String", "WS_URL", quoted(prop("krunknative.wsUrl")))
        buildConfigField("String", "ASSET_BASE_URL", quoted(prop("krunknative.assetBaseUrl")))
    }

    buildFeatures {
        buildConfig = true
    }

    buildTypes {
        release {
            isMinifyEnabled = false
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
// symlink), plus the selected part of <repo>/assets as web/assets/.
val copyWebClient by tasks.registering(Sync::class) {
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
