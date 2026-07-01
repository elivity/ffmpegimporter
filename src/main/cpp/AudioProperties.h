#ifndef FFMPEGIMPORTER_AUDIOPROPERTIES_H
#define FFMPEGIMPORTER_AUDIOPROPERTIES_H

#include "cstdint"

struct AudioProperties {
    int32_t channelCount;
    int32_t sampleRate;
};

#endif //FFMPEGIMPORTER_AUDIOPROPERTIES_H
