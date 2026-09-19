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

namespace {

    constexpr int kInternalBufferSize = 1152; // Matches the typical MP3 frame sample count.

/*
 * decode() already closed asset/targetData on its normal cleanup path in the
 * original implementation. Owning them with RAII makes the same ownership
 * rule apply to every early-return/error path as well.
 */
    struct FileCloser {
        void operator()(FILE *file) const noexcept {
            if (file != nullptr) {
                fclose(file);
            }
        }
    };

/*
 * AVIO owns the buffer passed to avio_alloc_context().
 *
 * FFmpeg is allowed to replace AVIOContext::buffer internally, therefore the
 * buffer which must be released is context->buffer at destruction time, not
 * necessarily the original pointer returned by av_malloc().
 */
    struct AVIOContextDeleter {
        void operator()(AVIOContext *context) const noexcept {
            if (context == nullptr) {
                return;
            }

            av_freep(&context->buffer);
            avio_context_free(&context);
        }
    };

/*
 * This is an INPUT AVFormatContext which has successfully passed
 * avformat_open_input().
 *
 * It must be closed through avformat_close_input(), not by directly calling
 * avformat_free_context().  close_input() gives the active demuxer a chance to
 * execute read_close() before the underlying AVFormatContext is released.
 *
 * With AVFMT_FLAG_CUSTOM_IO, avformat_close_input() deliberately does not free
 * our AVIOContext. ioContext below remains its independent owner.
 */
    struct AVFormatInputDeleter {
        void operator()(AVFormatContext *context) const noexcept {
            if (context == nullptr) {
                return;
            }

            AVFormatContext *tmp = context;
            avformat_close_input(&tmp);
        }
    };

    struct AVCodecContextDeleter {
        void operator()(AVCodecContext *context) const noexcept {
            if (context != nullptr) {
                avcodec_free_context(&context);
            }
        }
    };

    struct SwrContextDeleter {
        void operator()(SwrContext *context) const noexcept {
            if (context != nullptr) {
                swr_free(&context);
            }
        }
    };

    struct AVPacketDeleter {
        void operator()(AVPacket *packet) const noexcept {
            if (packet != nullptr) {
                av_packet_free(&packet);
            }
        }
    };

    struct AVFrameDeleter {
        void operator()(AVFrame *frame) const noexcept {
            if (frame != nullptr) {
                av_frame_free(&frame);
            }
        }
    };

} // namespace

int read(void *opaque, uint8_t *buf, int buf_size) {
    FILE *file = static_cast<FILE *>(opaque);

    if (file == nullptr || buf == nullptr || buf_size <= 0) {
        return AVERROR(EINVAL);
    }

    const size_t bytesRead = fread(
            buf,
            1,
            static_cast<size_t>(buf_size),
            file);

    if (bytesRead == 0) {
        if (feof(file)) {
            return AVERROR_EOF;
        }

        return AVERROR(errno != 0 ? errno : EIO);
    }

    return static_cast<int>(bytesRead);
}

int64_t seek(void *opaque, int64_t offset, int whence) {
    FILE *file = static_cast<FILE *>(opaque);

    if (file == nullptr) {
        return AVERROR(EINVAL);
    }

    const int normalizedWhence = whence & ~AVSEEK_FORCE;

    if (normalizedWhence == AVSEEK_SIZE) {
        const long currentPosition = ftell(file);
        if (currentPosition < 0) {
            return AVERROR(errno != 0 ? errno : EIO);
        }

        if (fseek(file, 0, SEEK_END) != 0) {
            return AVERROR(errno != 0 ? errno : EIO);
        }

        const long size = ftell(file);

        // Always try to restore the previous position.
        (void) fseek(file, currentPosition, SEEK_SET);

        if (size < 0) {
            return AVERROR(errno != 0 ? errno : EIO);
        }

        return static_cast<int64_t>(size);
    }

    /*
     * FFmpeg may OR AVSEEK_FORCE into whence. stdio only understands the
     * SEEK_SET/SEEK_CUR/SEEK_END part.
     */
    const int stdioWhence = normalizedWhence;

    if (stdioWhence != SEEK_SET &&
        stdioWhence != SEEK_CUR &&
        stdioWhence != SEEK_END) {
        return AVERROR(EINVAL);
    }

    if (fseek(file, static_cast<long>(offset), stdioWhence) != 0) {
        return AVERROR(errno != 0 ? errno : EIO);
    }

    const long position = ftell(file);
    if (position < 0) {
        return AVERROR(errno != 0 ? errno : EIO);
    }

    return static_cast<int64_t>(position);
}

bool FFMpegExtractor::createAVIOContext(
        FILE *asset,
        uint8_t *buffer,
        uint32_t bufferSize,
        AVIOContext **avioContext) {

    if (asset == nullptr ||
        buffer == nullptr ||
        bufferSize == 0 ||
        avioContext == nullptr) {
        LOGE("Invalid arguments while creating AVIOContext");
        return false;
    }

    constexpr int isBufferWritable = 0;

    *avioContext = avio_alloc_context(
            buffer,
            static_cast<int>(bufferSize),
            isBufferWritable,
            asset,
            read,
            nullptr,
            seek);

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

    if (avioContext == nullptr || avFormatContext == nullptr) {
        LOGE("Invalid arguments while creating AVFormatContext");
        return false;
    }

    *avFormatContext = avformat_alloc_context();

    if (*avFormatContext == nullptr) {
        LOGE("Failed to create AVFormatContext");
        return false;
    }

    (*avFormatContext)->pb = avioContext;

    /*
     * avformat_open_input() also sets this automatically when pb is already
     * populated, but setting it here makes the ownership rule explicit:
     * libavformat must never close/free our custom AVIOContext.
     */
    (*avFormatContext)->flags |= AVFMT_FLAG_CUSTOM_IO;

    return true;
}

bool FFMpegExtractor::openAVFormatContext(
        AVFormatContext *avFormatContext,
        std::string dest) {

    /*
     * Kept for source compatibility with the existing header.
     *
     * IMPORTANT:
     * decode() intentionally does NOT call this helper anymore.
     * avformat_open_input() takes AVFormatContext ** and can free + null a
     * caller-supplied context on failure. A pointer passed to this function by
     * value cannot propagate that null back to the real owner.
     *
     * The safe open path is implemented directly inside decode(), where the
     * raw pointer passed to avformat_open_input() is the actual owning
     * variable.
     */
    if (avFormatContext == nullptr) {
        LOGE("AVFormatContext is null");
        return false;
    }

    AVFormatContext *localContext = avFormatContext;

    const int result = avformat_open_input(
            &localContext,
            dest.c_str(),
            nullptr,
            nullptr);

    if (result == 0) {
        /*
         * A preallocated AVFormatContext is expected to remain the same object
         * on successful open. Detect a surprising replacement instead of
         * silently leaving the caller with the wrong pointer.
         */
        if (localContext != avFormatContext) {
            LOGE("avformat_open_input unexpectedly replaced AVFormatContext");
            if (localContext != nullptr) {
                avformat_close_input(&localContext);
            }
            return false;
        }

        return true;
    }

    /*
     * Do not touch avFormatContext here after failure: FFmpeg documents that a
     * caller-supplied AVFormatContext is freed on avformat_open_input failure.
     * This is why decode() no longer uses this legacy helper.
     */
    LOGE("Failed to open input. Error: %s", av_err2str(result));
    return false;
}

bool FFMpegExtractor::getStreamInfo(AVFormatContext *avFormatContext) {
    if (avFormatContext == nullptr) {
        LOGE("AVFormatContext is null");
        return false;
    }

    const int result = avformat_find_stream_info(
            avFormatContext,
            nullptr);

    if (result >= 0) {
        return true;
    }

    LOGE("Failed to find stream info. Error: %s", av_err2str(result));
    return false;
}

AVStream *FFMpegExtractor::getBestAudioStream(
        AVFormatContext *avFormatContext) {

    if (avFormatContext == nullptr) {
        LOGE("AVFormatContext is null");
        return nullptr;
    }

    const int streamIndex = av_find_best_stream(
            avFormatContext,
            AVMEDIA_TYPE_AUDIO,
            -1,
            -1,
            nullptr,
            0);

    if (streamIndex < 0) {
        LOGE("Could not find an audio stream. Error: %s",
             av_err2str(streamIndex));
        return nullptr;
    }

    if (static_cast<unsigned int>(streamIndex) >=
        avFormatContext->nb_streams) {
        LOGE("Audio stream index is out of bounds: %d / %u",
             streamIndex,
             avFormatContext->nb_streams);
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

    constexpr int64_t kDecodeError = -1;

    /*
     * The original function closed both FILE*s in its final cleanup block.
     * RAII preserves that ownership contract and now closes them on EVERY
     * return path, including failures before packet decoding starts.
     */
    std::unique_ptr<FILE, FileCloser> assetFile(asset);
    std::unique_ptr<FILE, FileCloser> targetFile(targetData);

    if (assetFile == nullptr) {
        LOGE("Input FILE is null");
        return kDecodeError;
    }

    if (targetFile == nullptr) {
        LOGE("Output FILE is null");
        return kDecodeError;
    }

    if (targetProperties.channelCount <= 0 ||
        targetProperties.sampleRate <= 0) {
        LOGE("Invalid output AudioProperties: channels=%d sampleRate=%d",
             targetProperties.channelCount,
             targetProperties.sampleRate);
        return kDecodeError;
    }

    /*
     * Allocate the initial custom-IO buffer.
     * Until avio_alloc_context succeeds, this pointer is ours.
     */
    auto *initialBuffer = reinterpret_cast<uint8_t *>(
            av_malloc(kInternalBufferSize));

    if (initialBuffer == nullptr) {
        LOGE("Failed to allocate internal FFmpeg buffer");
        return kDecodeError;
    }

    AVIOContext *rawIoContext = nullptr;

    if (!createAVIOContext(
            assetFile.get(),
            initialBuffer,
            kInternalBufferSize,
            &rawIoContext)) {

        // createAVIOContext did not take ownership when allocation failed.
        av_free(initialBuffer);
        return kDecodeError;
    }

    /*
     * Ownership of initialBuffer has now moved into AVIOContext.
     * AVIOContextDeleter will free context->buffer (which FFmpeg may replace)
     * and then the AVIOContext itself.
     */
    std::unique_ptr<AVIOContext, AVIOContextDeleter> ioContext(
            rawIoContext);

    /*
     * Allocate the input AVFormatContext.
     *
     * Do NOT put this pointer into a unique_ptr yet.
     * avformat_open_input() is explicitly allowed to free a caller-supplied
     * AVFormatContext and set the pointer to nullptr on failure.
     */
    AVFormatContext *rawFormatContext = nullptr;

    if (!createAVFormatContext(
            ioContext.get(),
            &rawFormatContext)) {
        return kDecodeError;
    }

    /*
     * This is the crucial ownership fix.
     *
     * Pass the actual owning raw pointer variable to avformat_open_input().
     * If opening fails, FFmpeg frees the format context and writes nullptr
     * back into rawFormatContext. There is therefore no stale unique_ptr left
     * behind which can call avformat_free_context() a second time.
     */
    const int openResult = avformat_open_input(
            &rawFormatContext,
            destination.c_str(),
            nullptr,
            nullptr);

    if (openResult < 0) {
        LOGE("Failed to open input. Error: %s", av_err2str(openResult));

        /*
         * FFmpeg's API contract says a caller-supplied AVFormatContext is
         * freed on failure and *ps is set to nullptr. Do not attempt another
         * free here: that is exactly the stale/double-free class of bug this
         * rewrite is designed to avoid.
         */
        if (rawFormatContext != nullptr) {
            LOGE("avformat_open_input failed but left a non-null context");
        }

        return kDecodeError;
    }

    /*
     * From this point the input is OPEN.
     * Its correct close operation is avformat_close_input(), not a direct
     * avformat_free_context().
     */
    std::unique_ptr<AVFormatContext, AVFormatInputDeleter> formatContext(
            rawFormatContext);

    if (!getStreamInfo(formatContext.get())) {
        return kDecodeError;
    }

    // Select the most suitable audio stream for decoding.
    AVStream *stream = getBestAudioStream(formatContext.get());

    if (stream == nullptr || stream->codecpar == nullptr) {
        LOGE("Could not find a suitable audio stream to decode");

        /*
         * Returning here is now safe:
         *
         *   formatContext -> avformat_close_input()
         *   ioContext     -> av_freep(buffer) + avio_context_free()
         *
         * The production crash happened on this exact early-return path when
         * formatContext used avformat_free_context directly as its deleter.
         */
        return kDecodeError;
    }

    printCodecParameters(stream->codecpar);

    const AVCodec *codec =
            avcodec_find_decoder(stream->codecpar->codec_id);

    if (codec == nullptr) {
        LOGE("Could not find codec with ID: %d",
             stream->codecpar->codec_id);
        return kDecodeError;
    }

    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> codecContext(
            avcodec_alloc_context3(codec));

    if (codecContext == nullptr) {
        LOGE("Failed to allocate codec context");
        return kDecodeError;
    }

    int result = avcodec_parameters_to_context(
            codecContext.get(),
            stream->codecpar);

    if (result < 0) {
        LOGE("Failed to copy codec parameters to codec context. Error: %s",
             av_err2str(result));
        return kDecodeError;
    }

    result = avcodec_open2(
            codecContext.get(),
            codec,
            nullptr);

    if (result < 0) {
        LOGE("Could not open codec. Error: %s",
             av_err2str(result));
        return kDecodeError;
    }

    std::unique_ptr<SwrContext, SwrContextDeleter> swr(
            swr_alloc());

    if (swr == nullptr) {
        LOGE("Failed to allocate resampler context");
        return kDecodeError;
    }

    AVChannelLayout inChannelLayout{};
    AVChannelLayout outChannelLayout{};

    const int inputChannelCount =
            stream->codecpar->ch_layout.nb_channels;

    if (inputChannelCount <= 0) {
        LOGE("Invalid input channel count: %d", inputChannelCount);
        return kDecodeError;
    }

    av_channel_layout_default(
            &inChannelLayout,
            inputChannelCount);

    av_channel_layout_default(
            &outChannelLayout,
            targetProperties.channelCount);

    result = av_opt_set_chlayout(
            swr.get(),
            "in_chlayout",
            &inChannelLayout,
            0);

    if (result >= 0) {
        result = av_opt_set_chlayout(
                swr.get(),
                "out_chlayout",
                &outChannelLayout,
                0);
    }

    /*
     * av_opt_set_chlayout() copies the layout value into SwrContext, so the
     * temporary local layouts can now be uninitialized.
     */
    av_channel_layout_uninit(&inChannelLayout);
    av_channel_layout_uninit(&outChannelLayout);

    if (result < 0) {
        LOGE("Failed to configure resampler channel layout. Error: %s",
             av_err2str(result));
        return kDecodeError;
    }

    result = av_opt_set_int(
            swr.get(),
            "in_sample_rate",
            stream->codecpar->sample_rate,
            0);

    if (result >= 0) {
        result = av_opt_set_int(
                swr.get(),
                "out_sample_rate",
                targetProperties.sampleRate,
                0);
    }

    if (result >= 0) {
        result = av_opt_set_sample_fmt(
                swr.get(),
                "in_sample_fmt",
                static_cast<AVSampleFormat>(
                        stream->codecpar->format),
                0);
    }

    if (result >= 0) {
        result = av_opt_set_sample_fmt(
                swr.get(),
                "out_sample_fmt",
                AV_SAMPLE_FMT_S16,
                0);
    }

    if (result >= 0) {
        result = av_opt_set_int(
                swr.get(),
                "force_resampling",
                1,
                0);
    }

    if (result < 0) {
        LOGE("Failed to configure resampler. Error: %s",
             av_err2str(result));
        return kDecodeError;
    }

    result = swr_init(swr.get());

    if (result < 0) {
        LOGE("swr_init failed. Error: %s", av_err2str(result));
        return kDecodeError;
    }

    if (!swr_is_initialized(swr.get())) {
        LOGE("Resampler was not initialized");
        return kDecodeError;
    }

    const int bytesPerSample = av_get_bytes_per_sample(
            static_cast<AVSampleFormat>(
                    stream->codecpar->format));

    LOGD("Input bytes per sample: %d", bytesPerSample);

    std::unique_ptr<AVPacket, AVPacketDeleter> avPacket(
            av_packet_alloc());

    std::unique_ptr<AVFrame, AVFrameDeleter> decodedFrame(
            av_frame_alloc());

    if (avPacket == nullptr || decodedFrame == nullptr) {
        LOGE("Failed to allocate packet or frame");
        return kDecodeError;
    }

    int64_t bytesWritten = 0;

    while ((result = av_read_frame(
            formatContext.get(),
            avPacket.get())) >= 0) {

        if (avPacket->stream_index == stream->index &&
            avPacket->size > 0) {

            result = avcodec_send_packet(
                    codecContext.get(),
                    avPacket.get());

            if (result < 0) {
                LOGE("avcodec_send_packet failed. Error: %s",
                     av_err2str(result));
                return kDecodeError;
            }

            /*
             * Preserve the existing importer behavior: consume the frame made
             * available by this packet. EAGAIN simply means the decoder needs
             * more compressed input.
             */
            result = avcodec_receive_frame(
                    codecContext.get(),
                    decodedFrame.get());

            if (result == AVERROR(EAGAIN)) {
                LOGI("Decoder requires more packet data");
                av_packet_unref(avPacket.get());
                continue;
            }

            if (result == AVERROR_EOF) {
                av_packet_unref(avPacket.get());
                break;
            }

            if (result < 0) {
                LOGE("avcodec_receive_frame failed. Error: %s",
                     av_err2str(result));
                return kDecodeError;
            }

            if (decodedFrame->sample_rate <= 0) {
                LOGE("Decoded frame has invalid sample rate: %d",
                     decodedFrame->sample_rate);
                return kDecodeError;
            }

            const int32_t dstNbSamples = static_cast<int32_t>(
                    av_rescale_rnd(
                            swr_get_delay(
                                    swr.get(),
                                    decodedFrame->sample_rate)
                            + decodedFrame->nb_samples,
                            targetProperties.sampleRate,
                            decodedFrame->sample_rate,
                            AV_ROUND_UP));

            if (dstNbSamples <= 0) {
                LOGE("Invalid destination sample count: %d",
                     dstNbSamples);
                return kDecodeError;
            }

            uint8_t *resampledBuffer = nullptr;

            result = av_samples_alloc(
                    &resampledBuffer,
                    nullptr,
                    targetProperties.channelCount,
                    dstNbSamples,
                    AV_SAMPLE_FMT_S16,
                    0);

            if (result < 0 || resampledBuffer == nullptr) {
                LOGE("Failed to allocate resampled audio buffer. Error: %s",
                     av_err2str(result));

                if (resampledBuffer != nullptr) {
                    av_freep(&resampledBuffer);
                }

                return kDecodeError;
            }

            const int frameCount = swr_convert(
                    swr.get(),
                    &resampledBuffer,
                    dstNbSamples,
                    const_cast<const uint8_t **>(
                            decodedFrame->data),
                    decodedFrame->nb_samples);

            if (frameCount < 0) {
                LOGE("swr_convert failed. Error: %s",
                     av_err2str(frameCount));
                av_freep(&resampledBuffer);
                return kDecodeError;
            }

            const int64_t bytesToWrite =
                    static_cast<int64_t>(frameCount)
                    * static_cast<int64_t>(sizeof(int16_t))
                    * static_cast<int64_t>(
                            targetProperties.channelCount);

            const size_t written = fwrite(
                    resampledBuffer,
                    1,
                    static_cast<size_t>(bytesToWrite),
                    targetFile.get());

            av_freep(&resampledBuffer);

            if (written != static_cast<size_t>(bytesToWrite)) {
                LOGE("Failed to write all audio data to the output file");
                return kDecodeError;
            }

            bytesWritten += static_cast<int64_t>(written);

            /*
             * The next receive should not observe stale frame references.
             */
            av_frame_unref(decodedFrame.get());
        }

        av_packet_unref(avPacket.get());
    }

    /*
     * AVERROR_EOF is the expected termination condition.
     * Other read errors are reported, but preserve the historical behavior of
     * returning the successfully decoded byte count collected before the read
     * ended.
     */
    if (result != AVERROR_EOF && result < 0) {
        LOGE("av_read_frame ended with: %s", av_err2str(result));
    }

    LOGI("FFmpeg decode complete. Bytes written: %" PRId64,
         bytesWritten);

    return bytesWritten;
}

void FFMpegExtractor::printCodecParameters(
        AVCodecParameters *params) {

    if (params == nullptr) {
        LOGE("Codec parameters are null");
        return;
    }

    LOGD("Stream properties");
    LOGD("Channel count: %d",
         params->ch_layout.nb_channels);
    LOGD("Sample rate: %d",
         params->sample_rate);

    const char *sampleFormatName =
            av_get_sample_fmt_name(
                    static_cast<AVSampleFormat>(
                            params->format));

    LOGD("Format: %s",
         sampleFormatName != nullptr
         ? sampleFormatName
         : "unknown");

    LOGD("Frame size: %d",
         params->frame_size);
}