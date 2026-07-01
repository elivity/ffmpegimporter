//
// Created by oskar on 26/4/25.
//

#ifndef RAP_RECORDER_HD_FFMPEGEXTRACTOR_H
#define RAP_RECORDER_HD_FFMPEGEXTRACTOR_H


#include <android/asset_manager.h>
#include "libavformat/avio.h"
#include "libavformat/avformat.h"

class FFMpegExtractor {
    bool createAVFormatContext(AVIOContext *avioContext, AVFormatContext **avFormatContext);

    bool getStreamInfo(AVFormatContext *avFormatContext);

    AVStream *getBestAudioStream(AVFormatContext *avFormatContext);

    static void printCodecParameters(AVCodecParameters *params);

    bool openAVFormatContext(AVFormatContext *avFormatContext, std::string dest);

public:
    struct AudioProperties {
        int32_t channelCount;
        int32_t sampleRate;
    };

    int64_t decode(AAsset *asset, uint8_t *targetData, AudioProperties targetProperties);

    int64_t decode(FILE *asset, FILE *targetData, std::string dest, AudioProperties targetProperties);

    bool
    createAVIOContext(FILE *asset, uint8_t *buffer, uint32_t bufferSize, AVIOContext **avioContext);
};


#endif //RAP_RECORDER_HD_FFMPEGEXTRACTOR_H
