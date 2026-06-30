package com.example.ffmpegimporter.importer

import com.example.ffmpegimporter.importer.SoAuditHelper.loadedSoPaths


enum class NativeBridgeImporter {
    INSTANCE
    ;

    companion object {
        var mEngineHandle: Long = 0

        fun getSoPath(filesDir: String) = "$filesDir/so"

        fun initSoFiles(filesDir: String, onError: ((Throwable) -> Unit)?) {
            println("oski loaded lib-1: $loadedSoPaths")
            FFmpegLoader.load(getSoPath(filesDir), onError)

            //System.loadLibrary("ffmpegimporter")
            println("oski loaded lib: $loadedSoPaths")
        }

        suspend fun execute(mEngineHandle: Long, filesDir: String, src: String, dest: String, sampleRate: Int, onError: ((Throwable) -> Unit)?) {
            initSoFiles(filesDir, onError)
            execute2(mEngineHandle, src, dest, sampleRate)
        }
        external fun execute2(mEngineHandle: Long, src: String, dest: String, sampleRate: Int)
    }
}
