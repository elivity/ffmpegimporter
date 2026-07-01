#include <android/log.h>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <string>
#include "logging/macros.h"
#include "AudioProperties.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "FFMpegExtractor.h"
#include "libavformat/avio.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libavutil/opt.h"
#include "libavutil/channel_layout.h"
#include "libavutil/samplefmt.h"
#include "libavutil/mem.h"
#include "libswresample/swresample.h"

#ifdef __cplusplus
}
#endif

#ifndef LOG_TAG
#define LOG_TAG "FFMpegImporter"
#endif

constexpr int kInternalBufferSize = 1152; // Matches the typical MP3 frame sample count.

int read(void *opaque, uint8_t *buf, int buf_size) {
    FILE *file = static_cast<FILE *>(opaque);
    size_t bytesRead = fread(buf, 1, static_cast<size_t>(buf_size), file);

    if (bytesRead == 0) {
        if (feof(file)) {
            return AVERROR_EOF;
        } else {
            return AVERROR(errno);
        }
    }

    return static_cast<int>(bytesRead);
}

int64_t seek(void *opaque, int64_t offset, int whence) {
    FILE *file = static_cast<FILE *>(opaque);

    if (whence == AVSEEK_SIZE) {
        long currentPosition = ftell(file);
        fseek(file, 0, SEEK_END);
        long size = ftell(file);
        fseek(file, currentPosition, SEEK_SET);
        return size;
    }

    if (fseek(file, static_cast<long>(offset), whence) < 0) {
        return -1;
    }

    return ftell(file);
}

bool FFMpegExtractor::createAVIOContext(
        FILE *asset,
        uint8_t *buffer,
        uint32_t bufferSize,
        AVIOContext **avioContext) {

    constexpr int isBufferWritable = 0;

    *avioContext = avio_alloc_context(
            buffer,
            bufferSize,
            isBufferWritable,
            asset,
            read,
            nullptr,
            seek
    );

    if (*avioContext == nullptr) {
        LOGE("Failed to create AVIO context");
        return false;
    }

    LOGV("AVIO context created");
    return true;
}

bool FFMpegExtractor::createAVFormatContext(
        AVIOContext *avioContext,
        AVFormatContext **avFormatContext) {

    *avFormatContext = avformat_alloc_context();

    if (*avFormatContext == nullptr) {
        LOGE("Failed to create AVFormatContext");
        return false;
    }

    (*avFormatContext)->pb = avioContext;
    return true;
}

bool FFMpegExtractor::openAVFormatContext(
        AVFormatContext *avFormatContext,
        std::string dest) {

    int result = avformat_open_input(
            &avFormatContext,
            dest.c_str(),
            nullptr,
            nullptr
    );

    if (result == 0) {
        return true;
    }

    LOGE("Failed to open input. Error: %s", av_err2str(result));
    return false;
}

bool FFMpegExtractor::getStreamInfo(AVFormatContext *avFormatContext) {
    if (avFormatContext == nullptr) {
        LOGE("AVFormatContext is null");
        return false;
    }

    int result = avformat_find_stream_info(avFormatContext, nullptr);

    if (result == 0) {
        return true;
    }

    LOGE("Failed to find stream info. Error: %s", av_err2str(result));
    return false;
}

AVStream *FFMpegExtractor::getBestAudioStream(AVFormatContext *avFormatContext) {
    int streamIndex = av_find_best_stream(
            avFormatContext,
            AVMEDIA_TYPE_AUDIO,
            -1,
            -1,
            nullptr,
            0
    );

    if (streamIndex < 0) {
        LOGE("Could not find an audio stream");
        return nullptr;
    }

    return avFormatContext->streams[streamIndex];
}

int64_t FFMpegExtractor::decode(
        FILE *asset,
        FILE *targetData,
        std::string destination,
        AudioProperties targetProperties) {

    LOGI("Decoder selected: FFmpeg");

    int returnValue = -1;

    // Allocate the internal buffer used by FFmpeg for input reads.
    auto buffer = reinterpret_cast<uint8_t *>(av_malloc(kInternalBufferSize));
    if (buffer == nullptr) {
        LOGE("Failed to allocate internal FFmpeg buffer");
        return returnValue;
    }

    // Create an AVIOContext with automatic cleanup.
    std::unique_ptr<AVIOContext, void (*)(AVIOContext *)> ioContext{
            nullptr,
            [](AVIOContext *context) {
                if (context != nullptr) {
                    av_free(context->buffer);
                    avio_context_free(&context);
                }
            }
    };

    {
        AVIOContext *tmp = nullptr;
        if (!createAVIOContext(asset, buffer, kInternalBufferSize, &tmp)) {
            LOGE("Could not create AVIOContext");
            av_free(buffer);
            return returnValue;
        }
        ioContext.reset(tmp);
    }

    // Create an AVFormatContext with automatic cleanup.
    std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> formatContext{
            nullptr,
            &avformat_free_context
    };

    {
        AVFormatContext *tmp = nullptr;
        if (!createAVFormatContext(ioContext.get(), &tmp)) {
            return returnValue;
        }
        formatContext.reset(tmp);
    }

    if (!openAVFormatContext(formatContext.get(), destination)) {
        return returnValue;
    }

    if (!getStreamInfo(formatContext.get())) {
        return returnValue;
    }

    // Select the most suitable audio stream for decoding.
    AVStream *stream = getBestAudioStream(formatContext.get());
    if (stream == nullptr || stream->codecpar == nullptr) {
        LOGE("Could not find a suitable audio stream to decode");
        return returnValue;
    }

    printCodecParameters(stream->codecpar);

    // Find the decoder for the selected audio stream.
    AVCodec *codec = const_cast<AVCodec *>(
            avcodec_find_decoder(stream->codecpar->codec_id)
    );

    if (codec == nullptr) {
        LOGE("Could not find codec with ID: %d", stream->codecpar->codec_id);
        return returnValue;
    }

    // Create the codec context with automatic cleanup.
    std::unique_ptr<AVCodecContext, void (*)(AVCodecContext *)> codecContext{
            nullptr,
            [](AVCodecContext *context) {
                avcodec_free_context(&context);
            }
    };

    {
        AVCodecContext *tmp = avcodec_alloc_context3(codec);
        if (tmp == nullptr) {
            LOGE("Failed to allocate codec context");
            return returnValue;
        }
        codecContext.reset(tmp);
    }

    // Copy stream codec parameters into the codec context.
    if (avcodec_parameters_to_context(codecContext.get(), stream->codecpar) < 0) {
        LOGE("Failed to copy codec parameters to codec context");
        return returnValue;
    }

    // Open the decoder.
    if (avcodec_open2(codecContext.get(), codec, nullptr) < 0) {
        LOGE("Could not open codec");
        return returnValue;
    }

    // Prepare the resampler.
    int32_t outChannelLayout = (1 << targetProperties.channelCount) - 1;
    LOGD("Output channel layout: %d", outChannelLayout);

    SwrContext *swr = swr_alloc();
    if (swr == nullptr) {
        LOGE("Failed to allocate resampler context");
        return returnValue;
    }

    AVChannelLayout inChannelLayout;
    AVChannelLayout outChannelLayoutConfig;

    av_channel_layout_default(
            &inChannelLayout,
            stream->codecpar->ch_layout.nb_channels
    );

    av_channel_layout_default(
            &outChannelLayoutConfig,
            targetProperties.channelCount
    );

    // Configure input and output channel layouts.
    av_opt_set_chlayout(swr, "in_chlayout", &inChannelLayout, 0);
    av_opt_set_chlayout(swr, "out_chlayout", &outChannelLayoutConfig, 0);

    // Configure sample rate and sample format conversion.
    av_opt_set_int(swr, "in_sample_rate", stream->codecpar->sample_rate, 0);
    av_opt_set_int(swr, "out_sample_rate", targetProperties.sampleRate, 0);
    av_opt_set_int(swr, "in_sample_fmt", stream->codecpar->format, 0);
    av_opt_set_sample_fmt(swr, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    av_opt_set_int(swr, "force_resampling", 1, 0);

    // Initialize and validate the resampler.
    int result = swr_init(swr);
    if (result != 0) {
        LOGE("swr_init failed. Error: %s", av_err2str(result));
        swr_free(&swr);
        return returnValue;
    }

    if (!swr_is_initialized(swr)) {
        LOGE("Resampler was not initialized");
        swr_free(&swr);
        return returnValue;
    }

    // Prepare packet and frame buffers for decoding.
    int bytesWritten = 0;

    int bytesPerSample = av_get_bytes_per_sample(
            static_cast<AVSampleFormat>(stream->codecpar->format)
    );

    LOGD("Input bytes per sample: %d", bytesPerSample);

    AVPacket *avPacket = av_packet_alloc();
    AVFrame *decodedFrame = av_frame_alloc();

    if (avPacket == nullptr || decodedFrame == nullptr) {
        LOGE("Failed to allocate packet or frame");
        goto cleanup;
    }

    LOGD("Input bytes per sample: %d", bytesPerSample);

    // Decode packets from the selected audio stream.
    while (av_read_frame(formatContext.get(), avPacket) == 0) {
        if (avPacket->stream_index == stream->index && avPacket->size > 0) {
            // Send compressed packet data to the decoder.
            result = avcodec_send_packet(codecContext.get(), avPacket);
            if (result != 0) {
                LOGE("avcodec_send_packet failed. Error: %s", av_err2str(result));
                goto cleanup;
            }

            // Receive decoded PCM frame data from the decoder.
            result = avcodec_receive_frame(codecContext.get(), decodedFrame);
            if (result == AVERROR(EAGAIN)) {
                LOGI("Decoder requires more packet data");
                av_packet_unref(avPacket);
                continue;
            } else if (result != 0) {
                LOGE("avcodec_receive_frame failed. Error: %s", av_err2str(result));
                goto cleanup;
            }

            // Resample decoded audio to the requested output format.
            auto dstNbSamples = static_cast<int32_t>(
                    av_rescale_rnd(
                            swr_get_delay(swr, decodedFrame->sample_rate)
                            + decodedFrame->nb_samples,
                            targetProperties.sampleRate,
                            decodedFrame->sample_rate,
                            AV_ROUND_UP
                    )
            );

            uint8_t *resampledBuffer = nullptr;

            result = av_samples_alloc(
                    &resampledBuffer,
                    nullptr,
                    targetProperties.channelCount,
                    dstNbSamples,
                    AV_SAMPLE_FMT_S16,
                    0
            );

            if (result < 0 || resampledBuffer == nullptr) {
                LOGE("Failed to allocate resampled audio buffer. Error: %s", av_err2str(result));
                goto cleanup;
            }

            int frameCount = swr_convert(
                    swr,
                    &resampledBuffer,
                    dstNbSamples,
                    const_cast<const uint8_t **>(decodedFrame->data),
                    decodedFrame->nb_samples
            );

            if (frameCount < 0) {
                LOGE("swr_convert failed. Error: %s", av_err2str(frameCount));
                av_freep(&resampledBuffer);
                goto cleanup;
            }

            int64_t bytesToWrite =
                    frameCount * sizeof(int16_t) * targetProperties.channelCount;

            size_t written = fwrite(
                    resampledBuffer,
                    1,
                    static_cast<size_t>(bytesToWrite),
                    targetData
            );

            av_freep(&resampledBuffer);

            if (written != static_cast<size_t>(bytesToWrite)) {
                LOGE("Failed to write all audio data to the output file");
                goto cleanup;
            }

            bytesWritten += static_cast<int>(written);
        }

        av_packet_unref(avPacket);
    }

    returnValue = bytesWritten;

    cleanup:
    if (avPacket != nullptr) {
        av_packet_free(&avPacket);
    }

    if (decodedFrame != nullptr) {
        av_frame_free(&decodedFrame);
    }

    if (swr != nullptr) {
        swr_free(&swr);
    }

    if (asset != nullptr) {
        fclose(asset);
    }

    if (targetData != nullptr) {
        fclose(targetData);
    }

    return returnValue;
}

void FFMpegExtractor::printCodecParameters(AVCodecParameters *params) {
    if (params == nullptr) {
        LOGE("Codec parameters are null");
        return;
    }

    LOGD("Stream properties");
    LOGD("Channel count: %d", params->ch_layout.nb_channels);
    LOGD("Sample rate: %d", params->sample_rate);
    LOGD("Format: %s", av_get_sample_fmt_name(static_cast<AVSampleFormat>(params->format)));
    LOGD("Frame size: %d", params->frame_size);
}