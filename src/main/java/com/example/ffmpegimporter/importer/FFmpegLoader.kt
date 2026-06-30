package com.example.ffmpegimporter.importer

import android.util.Log
import java.io.File

object FFmpegLoader {
    private const val TAG = "FFmpegLoader"
    private val LIBRARIES = arrayOf(
        "libavutil.so",
        "libavcodec.so",
        "libavformat.so",
        "libswscale.so",
        "libswresample.so"
    )

    @Throws(UnsatisfiedLinkError::class)
    fun load(customSoPath: String, onError: ((Throwable) -> Unit)?) {
        var allCustom = true

        for (lib in LIBRARIES) {
            val customLib = File(customSoPath, lib)
            if (customLib.exists()) {
                Log.i(TAG, "[✔] Loaded user library: " + customLib.absolutePath)
                try {
                    System.load(customLib.absolutePath)
                } catch (t: Throwable) {
                    onError?.invoke(t)
                }
            } else {
                allCustom = false
                /*throw UnsatisfiedLinkError(
                    "[✘] Missing required library: " + lib +
                            ". Expected in: " + filesDir.absolutePath
                )*/
            }
        }

        if (allCustom) {
            Log.i(TAG, "✅ All FFmpeg libraries loaded from user-provided files.")
        } else {
            Log.e(TAG, "⚠ Some FFmpeg libraries were missing. Check your setup!")
        }

        // Finally load your native bridge library (your app's JNI code)
        System.loadLibrary("ffmpegimporter")
    }
}