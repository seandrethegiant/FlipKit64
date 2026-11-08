// Android library module for AudiobookBridge soundscapes.
//
// Include it from the app:  settings.gradle.kts -> include(":soundscape")
//                           app/build.gradle.kts -> implementation(project(":soundscape"))
// Or copy src/main/java/... and src/main/assets/ambience into the app module.
//
// The plugin versions come from the root build (declare them there with
// `apply false`). Align the library versions below with the app's; nothing
// here is new to an app that already uses Media3 + Compose + coroutines.
plugins {
    id("com.android.library")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose") // Kotlin 2.x; on Kotlin 1.9 use composeOptions instead
}

android {
    namespace = "com.audiobookbridge.soundscape"
    compileSdk = 35

    defaultConfig {
        minSdk = 24
        consumerProguardFiles("consumer-rules.pro")
    }

    buildFeatures { compose = true }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    // Ogg files are already compressed; keep them uncompressed in the APK so
    // ExoPlayer can read them straight from the asset.
    androidResources { noCompress += "ogg" }

    testOptions { unitTests.isReturnDefaultValues = true }
}

dependencies {
    val media3 = "1.4.1"
    implementation("androidx.media3:media3-exoplayer:$media3")
    implementation("androidx.media3:media3-common:$media3")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.8.1")

    val composeBom = platform("androidx.compose:compose-bom:2024.09.03")
    implementation(composeBom)
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.foundation:foundation")

    testImplementation("junit:junit:4.13.2")
}
