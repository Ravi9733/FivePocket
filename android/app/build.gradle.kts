plugins {
    id("com.android.application")
}

// A separate package ID can be selected for the Turnip build without changing the installed app.
val appId = providers.gradleProperty("turnipApplicationId").orElse("dev.recompbootstrap.smoke").get()

// -PshipAssets=<absolute folder>: the staged game code for the "ship" build type. The folder holds ship/config.json
// and the files it names; android/scripts/stage_turnip_bundle.py makes it from an existing APK without changing it.
val shipAssets: String? = providers.gradleProperty("shipAssets").orNull

android {
    namespace = "dev.recompbootstrap.smoke"
    compileSdk = 35
    buildToolsVersion = "35.0.0"
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = appId
        minSdk = 26
        targetSdk = 35
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
        }
        release {
            isMinifyEnabled = false
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
        create("public") {
            initWith(getByName("release"))
            isDebuggable = false
            isJniDebuggable = false
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
                    arguments += "-DCMAKE_BUILD_TYPE=Debug"
                    arguments += "-DRECOMP_LOADER_OPEN_LOG=ON"
                }
            }
        }
    }

    sourceSets {
        getByName("public") { assets.srcDir("src/public/assets") }
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

tasks.matching { it.name == "preShipBuild" }.configureEach {
    val staged = shipAssets
    doFirst {
        if (staged == null || !File(staged, "ship/config.json").isFile) {
            throw GradleException("The ship build needs -PshipAssets=<absolute folder made by stage_turnip_bundle.py>; "
                    + "that folder must hold ship/config.json. Given: " + (staged ?: "nothing"))
        }
    }
}

tasks.matching { it.name == "prePublicBuild" }.configureEach {
    val source = file("src/public/assets")
    val main = file("src/main/assets")
    val prebuilt = listOf(file("src/main/jniLibs"), file("src/public/jniLibs"))
    val allowAssetFreePublicTest = project.hasProperty("publicTestSigned")
    doFirst {
        val expected = setOf("public/config.json", "public/start-request.bin")
        val found = source.walkTopDown().filter { it.isFile }.map { it.relativeTo(source).invariantSeparatorsPath }.toSet()
        if (found != expected && !(found.isEmpty() && allowAssetFreePublicTest))
            throw GradleException("The public app needs exactly its two metadata assets (or none for publicTestSigned).")
        val shared = main.walkTopDown().filter { it.isFile }.map { it.relativeTo(main).invariantSeparatorsPath }.toSet()
        if (shared != setOf("licenses/AMD-FidelityFX-FSR1-MIT.txt", "fonts/NunitoSans-VF.ttf", "licenses/NunitoSans-OFL.txt",
                        "licenses/GPL-3.0.txt"))
            throw GradleException("An unreviewed shared asset would enter the public app.")
        if (prebuilt.any { folder -> folder.walkTopDown().any { it.isFile } })
            throw GradleException("The public app cannot package prebuilt game libraries.")
    }
}
