plugins {
    id("com.android.application")
}

// -PshipAssets=<absolute folder>: the staged game code for the "ship" build type. The folder holds ship/config.json
// and the files it names; tools/ship_stage.py makes it (BUILD.md has the commands). assembleShip refuses to run
// without it; the other build types never look at it.
val shipAssets: String? = providers.gradleProperty("shipAssets").orNull

android {
    namespace = "dev.recompbootstrap.smoke"
    compileSdk = 35
    buildToolsVersion = "35.0.0"
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "dev.recompbootstrap.smoke"
        minSdk = 26
        targetSdk = 35
        // Stays 1 on purpose: an app that is not debuggable cannot be downgraded without an uninstall, and an
        // uninstall removes the game data and the saves. With the same version code the debug build can still be
        // installed over the ship build (and the other way round) with "adb install -r", data kept.
        versionCode = 1
        versionName = "1.0.2-alpha"

        ndk {
            abiFilters += "arm64-v8a"
        }

        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
                cppFlags += listOf("-std=c++17", "-Wall", "-Wextra", "-Wpedantic")
            }
        }
    }

    buildTypes {
        debug {
            // The loader (librecomp_private_debug.so) is a normal CMake target since ship week: no switch here.
        }
        release {
            isMinifyEnabled = false
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
        // The build that is handed out: not debuggable, game code inside (see shipAssets above), and signed with the
        // key the installed app carries today (the debug keystore, certificate SHA-256 f0ab38fa...37b0), so that it
        // installs over that app without an uninstall. BUILD.md says what a release key would change.
        // Public APK: only our loader/menu and text records. Never inherits the ship asset directory.
        create("public") {
            initWith(getByName("release"))
            isDebuggable = false
            isJniDebuggable = false
            // Unsigned by default: what is published is signed with a key of the owner's choosing. For test installs
            // -PpublicTestSigned makes Gradle sign it with the key the installed app carries today (the debug keystore,
            // as the ship build), so it installs over that app; nobody types a key or a password for that.
            signingConfig = if (project.hasProperty("publicTestSigned")) signingConfigs.getByName("debug") else null
            matchingFallbacks += "release"
            externalNativeBuild { cmake { arguments += "-DCMAKE_BUILD_TYPE=Debug" } }
        }
        create("ship") {
            initWith(getByName("release"))
            isDebuggable = false
            isJniDebuggable = false
            signingConfig = signingConfigs.getByName("debug")
            matchingFallbacks += "release"
            externalNativeBuild {
                cmake {
                    // The qualified loader is a Debug build (-O0). Shipping the same code generation keeps the
                    // loader's machine code equal to the one every qualified session ran with; an optimised
                    // loader is a separate, measured change.
                    arguments += "-DCMAKE_BUILD_TYPE=Debug"
                    // The loader's three log lines about opening the game library (CMakeLists.txt). Remove this line to
                    // ship the loader without them; ship_checks.sh apk then fails its line about them, on purpose.
                    arguments += "-DRECOMP_LOADER_OPEN_LOG=ON"
                }
            }
        }
    }

    sourceSets {
        getByName("public") { assets.srcDir("src/public/assets") }
        // The game window, its launcher and its gamepad input live in src/debug/java for historical reasons (they
        // were debug-only until ship week). They are compiled into every build type now; the folder keeps its name
        // because test scripts and LauncherContractTest read it by path. The debug source set itself adds no Java.
        getByName("main") {
            java.srcDir("src/debug/java")
        }
        getByName("debug") {
            java.setSrcDirs(emptyList<String>())
        }
        getByName("ship") {
            if (shipAssets != null) {
                assets.srcDir(shipAssets)
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Required by libadrenotools: hook libraries must be extracted into nativeLibraryDir
    // so the linker-namespace hook can load them from the exact ApplicationInfo path.
    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    buildFeatures {
        buildConfig = false
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

// A ship APK without game code would install and then have nothing to start: refuse to build one.
tasks.matching { it.name == "preShipBuild" }.configureEach {
    val staged = shipAssets
    doFirst {
        if (staged == null || !File(staged, "ship/config.json").isFile) {
            throw GradleException("The ship build needs -PshipAssets=<absolute folder made by ship_stage.py>;"
                    + " that folder must hold ship/config.json. Given: " + (staged ?: "nothing"))
        }
    }
}

// Source-set isolation is checked before packaging, including accidental additions to main/assets.
tasks.matching { it.name == "prePublicBuild" }.configureEach {
    // The folders are resolved here, not inside doFirst: a task action that calls the script's own file() cannot be
    // kept by Gradle's configuration cache, and the build then ends as failed although every task ran (2026-10-08).
    val source = file("src/public/assets")
    val main = file("src/main/assets")
    val prebuilt = listOf(file("src/main/jniLibs"), file("src/public/jniLibs"))
    val allowAssetFreePublicTest = project.hasProperty("publicTestSigned")
    doFirst {
        val expected = setOf("public/config.json", "public/start-request.bin")
        val found = source.walkTopDown().filter { it.isFile }.map { it.relativeTo(source).invariantSeparatorsPath }.toSet()
        // A signed publicTestSigned build is an asset-free loader/menu smoke APK. Without the
        // two staged metadata records it deliberately opens the developer menu and cannot start a game.
        if (found != expected && !(found.isEmpty() && allowAssetFreePublicTest))
            throw GradleException("The public app needs exactly its two metadata assets (or none for publicTestSigned).")
        val shared = main.walkTopDown().filter { it.isFile }.map { it.relativeTo(main).invariantSeparatorsPath }.toSet()
        // menu-redesign 2026-10-25 (the owner: "implement it with Nunito Sans"): the menus' typeface and its OFL licence, reviewed.
        // 2026-11-05 (owner-approved): the GPLv3 text the app itself is under (the repository's COPYING, byte for byte).
        if (shared != setOf("licenses/AMD-FidelityFX-FSR1-MIT.txt", "fonts/NunitoSans-VF.ttf", "licenses/NunitoSans-OFL.txt",
                        "licenses/GPL-3.0.txt"))
            throw GradleException("An unreviewed shared asset would enter the public app.")
        if (prebuilt.any { folder -> folder.walkTopDown().any { it.isFile } })
            throw GradleException("The public app cannot package prebuilt game libraries.")
    }
}
