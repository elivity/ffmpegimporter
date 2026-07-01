//
// Created by oskar on 26/4/25.
//
#include<cstdlib>
#include<cstring>
#include<string>
#include <jni.h>
#include "FFMpegExtractor.h"
#include "fstream"
#include "AudioProperties.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
}

extern "C"
JNIEXPORT void JNICALL
Java_com_example_ffmpegimporter_importer_NativeBridgeImporter_00024Companion_execute2(JNIEnv *env,
                                                                                      jobject thiz,
                                                                                      jlong m_engine_handle,
                                                                                      jstring srcPath,
                                                                                      jstring destPath,
                                                                                      jint sampleRate){
    const char *rawStringIn = env->GetStringUTFChars(srcPath, nullptr);
    const char *rawStringOut = env->GetStringUTFChars(destPath, nullptr);

    FILE* file = std::fopen(rawStringIn, "r");
    FILE* fileOut = std::fopen(rawStringOut, "w+");

    auto extractor = FFMpegExtractor();

    AudioProperties properties = AudioProperties();
    properties.channelCount = 2;
    properties.sampleRate = sampleRate;
    extractor.decode(file, fileOut, rawStringIn, properties);

    env->ReleaseStringUTFChars(srcPath, rawStringIn);
    env->DeleteLocalRef(srcPath);
    env->ReleaseStringUTFChars(destPath, rawStringOut);
    env->DeleteLocalRef(destPath);

}