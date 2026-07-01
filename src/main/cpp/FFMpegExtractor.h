//
// Created by oskar on 26/4/25.
//

#ifndef RAP_RECORDER_HD_FFMPEGEXTRACTOR_H
#define RAP_RECORDER_HD_FFMPEGEXTRACTOR_H


#include <android/asset_manager.h>
#include "libavformat/avio.h"
#include "libavformat/avformat.h"
#include "AudioProperties.h"

class FFMpegExtractor {
    bool createAVFormatContext(AVIOContext *avioContext, AVFormatContext **avFormatContext);

    bool getStreamInfo(AVFormatContext *avFormatContext);

    AVStream *getBestAudioStream(AVFormatContext *avFormatContext);

    static void printCodecParameters(AVCodecParameters *params);

    bool openAVFormatContext(AVFormatContext *avFormatContext, std::string dest);

public:
    int64_t decode(FILE *asset, FILE *targetData, std::string dest, AudioProperties targetProperties);

    bool
    createAVIOContext(FILE *asset, uint8_t *buffer, uint32_t bufferSize, AVIOContext **avioContext);
};


#endif //RAP_RECORDER_HD_FFMPEGEXTRACTOR_H
