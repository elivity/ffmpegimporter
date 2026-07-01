import java.io.File
import java.util.Properties
import org.gradle.api.publish.maven.MavenPublication

plugins {
    alias(libs.plugins.android.library)
    alias(libs.plugins.compose.compiler)
    `maven-publish`
}

group = "com.github.elivity"
version = "1.0.0"


val ffmpegAbisProvider = providers.gradleProperty("ffmpegAbis")
    .orElse("armeabi-v7a,arm64-v8a,x86,x86_64")

val ffmpegApiProvider = providers.gradleProperty("ffmpegApi")
    .orElse("21")

val ffmpegVersionProvider = providers.gradleProperty("ffmpegVersion")
    .orElse("7.1.1")

val ffmpegJniLibsDir = layout.projectDirectory.dir("src/main/jniLibs")
val ffmpegHeadersDir = layout.projectDirectory.dir("src/main/cpp/ffmpeg/include")

android {
    namespace = "com.raponmp3.ffmpegimporter"
    compileSdk = 36

    defaultConfig {
        minSdk = 21

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        consumerProguardFiles("consumer-rules.pro")

        ndkVersion = "27.2.12479018"

        externalNativeBuild {
            cmake {
                version = "3.22.1"
                arguments(
                    "-DFFMPEG_JNI_LIBS_DIR=${ffmpegJniLibsDir.asFile.absolutePath}",
                    "-DFFMPEG_INCLUDE_DIR=${ffmpegHeadersDir.asFile.absolutePath}"
                )
            }
        }
    }

    sourceSets {
        getByName("main") {
            jniLibs.srcDirs(ffmpegJniLibsDir.asFile)
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    publishing {
        singleVariant("release") {
            withSourcesJar()
        }
    }

    buildTypes {
        getByName("release") {
            isMinifyEnabled = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildFeatures {
        compose = true
    }
}

dependencies {
    implementation(libs.androidx.core.ktx)
    implementation(libs.androidx.appcompat)
    implementation(libs.material)

    implementation(platform(libs.androidx.compose.bom))
    androidTestImplementation(platform(libs.androidx.compose.bom))

    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.compose.runtime)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.foundation)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)

    debugImplementation(libs.androidx.compose.ui.tooling)
    debugImplementation(libs.androidx.compose.ui.test.manifest)

    testImplementation(libs.junit)
    androidTestImplementation(libs.androidx.compose.ui.test.junit4)
    androidTestImplementation(libs.androidx.test.ext.junit)
    androidTestImplementation(libs.androidx.test.espresso.core)
}

val buildFfmpegAndroid = tasks.register("buildFfmpegAndroid") {
    group = "ffmpeg"
    description = "Clone/update ffmpeg-android-maker and build FFmpeg Android shared libraries."

    inputs.property("ffmpegAbis", ffmpegAbisProvider)
    inputs.property("ffmpegApi", ffmpegApiProvider)
    inputs.property("ffmpegVersion", ffmpegVersionProvider)

    outputs.dir(ffmpegJniLibsDir)
    outputs.dir(ffmpegHeadersDir)

    doLast {
        val makerRepo = "https://github.com/Javernaut/ffmpeg-android-maker.git"
        val makerDir = rootProject.file("third_party/ffmpeg-android-maker")

        val androidSdkDir = findAndroidSdkDir()
        val androidNdkDir = findAndroidNdkDir(androidSdkDir)

        val abis = ffmpegAbisProvider.get()
        val androidApi = ffmpegApiProvider.get()
        val ffmpegVersion = ffmpegVersionProvider.get()

        println("FFmpeg Android Maker dir: ${makerDir.absolutePath}")
        println("Android SDK: ${androidSdkDir.absolutePath}")
        println("Android NDK: ${androidNdkDir.absolutePath}")
        println("ABIs: $abis")
        println("Android API: $androidApi")
        println("FFmpeg version: $ffmpegVersion")

        if (!File(makerDir, ".git").exists()) {
            makerDir.parentFile.mkdirs()

            runCommand(
                workingDir = rootProject.projectDir,
                "git",
                "clone",
                "--depth",
                "1",
                makerRepo,
                makerDir.absolutePath
            )
        } else {
            runCommand(
                workingDir = makerDir,
                "git",
                "fetch",
                "--depth",
                "1",
                "origin",
                "master"
            )

            runCommand(
                workingDir = makerDir,
                "git",
                "reset",
                "--hard",
                "origin/master"
            )
        }

        val exportBuildVariables = File(makerDir, "scripts/export-build-variables.sh")
        if (!exportBuildVariables.exists()) {
            error("Could not find ${exportBuildVariables.absolutePath}")
        }

        var exportText = exportBuildVariables.readText()

        exportText = exportText.replace(
            "export FFMPEG_EXTRA_LD_FLAGS=",
            "export FFMPEG_EXTRA_LD_FLAGS=\"\${FFMPEG_IMPORTER_EXTRA_LD_FLAGS:-}\""
        )

        exportText = exportText.replace(
            "export EXTRA_BUILD_CONFIGURATION_FLAGS=",
            "export EXTRA_BUILD_CONFIGURATION_FLAGS=\"\${FFMPEG_IMPORTER_EXTRA_CONFIG_FLAGS:-}\""
        )

        exportBuildVariables.writeText(exportText)

        runCommand(
            workingDir = makerDir,
            "chmod",
            "+x",
            "ffmpeg-android-maker.sh"
        )

        val extraConfigureFlags = listOf(
            "--disable-doc",
            "--disable-programs",
            "--disable-symver",
            "--disable-avdevice",
            "--disable-postproc",
            "--disable-network",
            "--disable-x86asm",
            "--enable-small"
        ).joinToString(" ")

        runCommand(
            workingDir = makerDir,
            "bash",
            "./ffmpeg-android-maker.sh",
            "--target-abis=$abis",
            "--android-api-level=$androidApi",
            "--source-tar=$ffmpegVersion",
            env = mapOf(
                "ANDROID_SDK_HOME" to androidSdkDir.absolutePath,
                "ANDROID_HOME" to androidSdkDir.absolutePath,
                "ANDROID_NDK_HOME" to androidNdkDir.absolutePath,
                "FFMPEG_IMPORTER_EXTRA_CONFIG_FLAGS" to extraConfigureFlags,
                "FFMPEG_IMPORTER_EXTRA_LD_FLAGS" to ""
            )
        )

        val generatedLibDir = File(makerDir, "output/lib")
        val generatedIncludeDir = File(makerDir, "output/include")

        val targetJniLibsDir = ffmpegJniLibsDir.asFile
        val targetHeadersDir = ffmpegHeadersDir.asFile

        if (!generatedLibDir.exists()) {
            error("FFmpeg build output not found: ${generatedLibDir.absolutePath}")
        }

        if (!generatedIncludeDir.exists()) {
            error("FFmpeg include output not found: ${generatedIncludeDir.absolutePath}")
        }

        delete(targetJniLibsDir)
        delete(targetHeadersDir)

        copy {
            from(generatedLibDir)
            into(targetJniLibsDir)
        }

        copy {
            from(generatedIncludeDir)
            into(targetHeadersDir)
        }

        println("FFmpeg .so files copied to: ${targetJniLibsDir.absolutePath}")
        println("FFmpeg headers copied to: ${targetHeadersDir.absolutePath}")
    }
}

/*
 * Make sure FFmpeg exists before Android/CMake/publishing tasks run.
 * Because buildFfmpegAndroid has inputs/outputs, Gradle should skip it when unchanged.
 */
tasks.matching { task ->
    task.name == "preBuild" ||
            task.name.startsWith("configureCMake") ||
            task.name.startsWith("externalNativeBuild") ||
            task.name.startsWith("merge") && task.name.endsWith("JniLibFolders") ||
            task.name == "bundleReleaseAar" ||
            task.name == "publishToMavenLocal" ||
            task.name.startsWith("publishReleasePublication")
}.configureEach {
    dependsOn(buildFfmpegAndroid)
}

afterEvaluate {
    publishing {
        publications {
            create<MavenPublication>("release") {
                from(components["release"])

                groupId = project.group.toString()
                artifactId = "ffmpegimporter"
                version = project.version.toString()

                pom {
                    name.set("FFmpeg Importer")
                    description.set("Android library wrapper that bundles FFmpeg native binaries.")
                }
            }
        }
    }
}

fun Project.localProperties(): Properties {
    val props = Properties()
    val file = rootProject.file("local.properties")
    if (file.exists()) {
        file.inputStream().use { props.load(it) }
    }
    return props
}

fun Project.findAndroidSdkDir(): File {
    val props = localProperties()

    val candidates = listOfNotNull(
        props.getProperty("sdk.dir"),
        System.getenv("ANDROID_SDK_HOME"),
        System.getenv("ANDROID_HOME")
    ).map { File(it) }

    return candidates.firstOrNull { it.exists() }
        ?: error(
            "Android SDK not found. Set sdk.dir in local.properties " +
                    "or ANDROID_SDK_HOME / ANDROID_HOME."
        )
}

fun Project.findAndroidNdkDir(androidSdkDir: File): File {
    val props = localProperties()

    val explicitCandidates = listOfNotNull(
        props.getProperty("ndk.dir"),
        System.getenv("ANDROID_NDK_HOME")
    ).map { File(it) }

    explicitCandidates.firstOrNull { it.exists() }?.let { return it }

    val ndkRoot = File(androidSdkDir, "ndk")
    return ndkRoot
        .listFiles()
        ?.filter { it.isDirectory }
        ?.maxByOrNull { it.name }
        ?: error(
            "Android NDK not found. Set ndk.dir in local.properties " +
                    "or ANDROID_NDK_HOME."
        )
}

fun runCommand(
    workingDir: File,
    vararg command: String,
    env: Map<String, String> = emptyMap()
) {
    println("Running: ${command.joinToString(" ")}")

    val process = ProcessBuilder(*command)
        .directory(workingDir)
        .redirectErrorStream(true)
        .apply {
            environment().putAll(env)
        }
        .start()

    process.inputStream.bufferedReader().useLines { lines ->
        lines.forEach { println(it) }
    }

    val exitCode = process.waitFor()
    if (exitCode != 0) {
        error("Command failed with exit code $exitCode: ${command.joinToString(" ")}")
    }
}
