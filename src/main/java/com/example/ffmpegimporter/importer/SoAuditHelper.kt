package com.example.ffmpegimporter.importer

import android.os.Process
import android.util.Log
import java.io.BufferedReader
import java.io.File
import java.io.FileReader
import java.io.IOException


object SoAuditHelper {
    private const val TAG = "SoAuditHelper"

    val loadedSoPaths: List<String>
        get() {
            val pid = Process.myPid()
            val mapsFile = File("/proc/$pid/maps")
            val soFiles: MutableList<String> = ArrayList()

            if (!mapsFile.exists()) {
                Log.e(TAG, "Error: /proc/$pid/maps does not exist!")
                return soFiles // return empty list instead of throwing
            }

            try {
                BufferedReader(FileReader(mapsFile)).use { reader ->
                    var line: String
                    while ((reader.readLine().also { line = it }) != null) {
                        if (line.contains(".so") && !line.contains("[anon]")) {
                            val parts =
                                line.trim { it <= ' ' }.split("\\s+".toRegex())
                                    .dropLastWhile { it.isEmpty() }
                                    .toTypedArray()
                            for (part in parts) {
                                if (part.endsWith(".so") && !soFiles.contains(part)) {
                                    soFiles.add(part)
                                }
                            }
                        }
                    }
                }
            } catch (e: IOException) {
                Log.e(TAG, "IOException while reading maps", e)
            } catch (e: Exception) {
                Log.e(TAG, "Unexpected error while reading maps", e)
            }

            return soFiles
        }

    fun logLoadedSoPaths() {
        val soFiles = loadedSoPaths
        Log.i(TAG, "=== Loaded .so Libraries ===")
        for (path in soFiles) {
            Log.i(TAG, path)
        }
        Log.i(TAG, "============================")
    }
}