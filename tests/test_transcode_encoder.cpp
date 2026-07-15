/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
#include "Codec/Transcode.h"
#include "Extension/Factory.h"
#include "ext-codec/H264.h"
#define private public
#define protected public
#include "Player/PlayerProxy.h"
#undef protected
#undef private

using namespace std;

int main() {
#if defined(ENABLE_FFMPEG)
    using namespace mediakit;

    auto make_config = [](int width, int height) {
        VideoEncodeConfig config;
        config.codec = CodecJPEG;
        config.width = width;
        config.height = height;
        config.fps = 1;
        config.bitrate = 512 * 1024;
        config.gop = 1;
        config.max_b_frames = 0;
        config.pixel_format = AV_PIX_FMT_YUVJ420P;
        config.preset.clear();
        config.zerolatency = false;
        return config;
    };

    auto config = make_config(64, 64);
    if (!config.profile.empty()) {
        cerr << "unexpected default encoder profile: " << config.profile << endl;
        return 1;
    }

    auto frame = std::make_shared<FFmpegFrame>();
    frame->get()->format = config.pixel_format;
    frame->get()->width = config.width;
    frame->get()->height = config.height;
    frame->get()->pts = 0;
    frame->get()->pkt_dts = 0;
    frame->fillPicture(config.pixel_format, config.width, config.height);

    for (int y = 0; y < config.height; ++y) {
        memset(frame->get()->data[0] + y * frame->get()->linesize[0], y * 4, config.width);
    }
    for (int y = 0; y < config.height / 2; ++y) {
        memset(frame->get()->data[1] + y * frame->get()->linesize[1], 128, config.width / 2);
        memset(frame->get()->data[2] + y * frame->get()->linesize[2], 128, config.width / 2);
    }

    size_t encoded_bytes = 0;
    vector<Frame::Ptr> encoded_frames;
    FFmpegEncoder encoder;
    encoder.setOnEncode([&](const Frame::Ptr &encoded) {
        if (!encoded) {
            return;
        }
        encoded_bytes += encoded->size();
        if (encoded->getCodecId() != CodecJPEG) {
            cerr << "unexpected codec: " << getCodecName(encoded->getCodecId()) << endl;
            exit(2);
        }
        encoded_frames.emplace_back(encoded);
    });

    encoder.open(config);
    for (int i = 0; i < 3; ++i) {
        frame->get()->pts = i * 1000;
        frame->get()->pkt_dts = i * 1000;
        if (!encoder.inputFrame(frame)) {
            cerr << "input frame failed" << endl;
            return 1;
        }
    }
    encoder.flush();

    if (!encoded_bytes) {
        cerr << "encoder produced no packet" << endl;
        return 1;
    }
    cout << "encoded bytes: " << encoded_bytes << endl;

    if (avcodec_find_encoder(AV_CODEC_ID_H264)) {
        VideoEncodeConfig h264_config;
        h264_config.width = config.width;
        h264_config.height = config.height;
        h264_config.fps = 25;
        h264_config.bitrate = config.bitrate;
        h264_config.gop = 1;
        h264_config.pixel_format = AV_PIX_FMT_YUV420P;

        vector<Frame::Ptr> h264_frames;
        FFmpegEncoder h264_encoder;
        h264_encoder.setOnEncode([&](const Frame::Ptr &encoded) {
            h264_frames.emplace_back(encoded);
        });
        h264_encoder.open(h264_config);
        frame->get()->pts = 0;
        frame->get()->pkt_dts = 0;
        if (!h264_encoder.inputFrame(frame)) {
            cerr << "H264 encoder input frame failed" << endl;
            return 1;
        }
        h264_encoder.flush();

        auto h264_track = Factory::getTrackByCodecId(CodecH264);
        bool saw_config = false;
        bool saw_key = false;
        for (auto &encoded : h264_frames) {
            if (!encoded || encoded->getCodecId() != CodecH264 || encoded->prefixSize() == 0) {
                cerr << "invalid H264 encoded frame" << endl;
                return 1;
            }
            size_t nal_count = 0;
            splitH264(encoded->data(), encoded->size(), encoded->prefixSize(),
                      [&](const char *, size_t, size_t) {
                ++nal_count;
            });
            if (nal_count != 1) {
                cerr << "H264 output frame contains " << nal_count << " NAL units" << endl;
                return 1;
            }
            saw_config = saw_config || encoded->configFrame();
            saw_key = saw_key || encoded->keyFrame();
            h264_track->inputFrame(encoded);
        }
        if (h264_frames.empty() || !saw_config || !saw_key || !h264_track->ready()) {
            cerr << "H264 output did not expose config/key frames" << endl;
            return 1;
        }

        size_t decoded_frames = 0;
        FFmpegDecoder h264_decoder(h264_track, 1, {"h264"});
        h264_decoder.setOnDecode([&](const FFmpegFrame::Ptr &) {
            ++decoded_frames;
        });
        for (auto &encoded : h264_frames) {
            if (!h264_decoder.inputFrame(encoded, false, false, true)) {
                cerr << "H264 decoder input frame failed" << endl;
                return 1;
            }
        }
        h264_decoder.flush();
        if (!decoded_frames) {
            cerr << "H264 decoder flush lost the final access unit" << endl;
            return 1;
        }
        cout << "H264 NAL frames: " << h264_frames.size() << endl;
    } else {
        cout << "H264 encoder unavailable, skip" << endl;
    }

    if (encoded_frames.empty()) {
        cerr << "encoder produced no frame" << endl;
        return 1;
    }

    auto input_track = std::make_shared<VideoTrackImp>(CodecJPEG, config.width, config.height, config.fps);
    auto output_config = make_config(32, 32);
    size_t transcoded_bytes = 0;
    FFmpegVideoTranscoder transcoder(input_track, output_config, 1);
    transcoder.setOnOutput([&](const Frame::Ptr &encoded) {
        if (!encoded) {
            return;
        }
        transcoded_bytes += encoded->size();
        if (encoded->getCodecId() != CodecJPEG) {
            cerr << "unexpected transcoded codec: " << getCodecName(encoded->getCodecId()) << endl;
            exit(3);
        }
    });
    for (auto &encoded_frame : encoded_frames) {
        if (!transcoder.inputFrame(encoded_frame, false, false)) {
            cerr << "transcoder input frame failed" << endl;
            return 1;
        }
    }
    transcoder.flush();

    auto output_track = transcoder.getOutputTrack();
    if (!output_track || !output_track->ready()) {
        cerr << "transcoder output track is not ready" << endl;
        return 1;
    }
    auto output_video = std::static_pointer_cast<VideoTrack>(output_track);
    if (output_video->getVideoWidth() != output_config.width || output_video->getVideoHeight() != output_config.height) {
        cerr << "unexpected output size: " << output_video->getVideoWidth() << "x" << output_video->getVideoHeight() << endl;
        return 1;
    }
    if (!transcoded_bytes) {
        cerr << "transcoder produced no packet" << endl;
        return 1;
    }
    cout << "transcoded bytes: " << transcoded_bytes << endl;

    class CollectSink : public MediaSinkInterface {
    public:
        explicit CollectSink(toolkit::EventPoller::Ptr poller) : owner_poller(std::move(poller)) {}

        bool addTrack(const Track::Ptr &track) override {
            wrong_thread = wrong_thread || !owner_poller->isCurrentThread();
            tracks.emplace_back(track);
            return true;
        }

        void addTrackCompleted() override {
            wrong_thread = wrong_thread || !owner_poller->isCurrentThread();
            completed = true;
        }

        void resetTracks() override {
            wrong_thread = wrong_thread || !owner_poller->isCurrentThread();
            tracks.clear();
            frames.clear();
            completed = false;
        }

        bool inputFrame(const Frame::Ptr &frame) override {
            wrong_thread = wrong_thread || !owner_poller->isCurrentThread();
            frames.emplace_back(frame);
            return true;
        }

    public:
        bool completed = false;
        bool wrong_thread = false;
        toolkit::EventPoller::Ptr owner_poller;
        vector<Track::Ptr> tracks;
        vector<Frame::Ptr> frames;
    };

    auto delegate_poller = toolkit::EventPollerPool::Instance().getPoller();
    auto collect_sink = std::make_shared<CollectSink>(delegate_poller);
    auto transcode_sink = std::make_shared<FFmpegVideoTranscodeSink>(collect_sink,
                                                                    output_config,
                                                                    1,
                                                                    vector<string>{},
                                                                    delegate_poller);
    if (!transcode_sink->addTrack(input_track)) {
        cerr << "transcode sink add track failed" << endl;
        return 1;
    }
    transcode_sink->addTrackCompleted();
    for (auto &encoded_frame : encoded_frames) {
        if (!transcode_sink->inputFrame(encoded_frame)) {
            cerr << "transcode sink input frame failed" << endl;
            return 1;
        }
    }
    transcode_sink->flush();
    if (collect_sink->wrong_thread) {
        cerr << "transcode sink invoked delegate from the wrong thread" << endl;
        return 1;
    }
    if (!collect_sink->completed || collect_sink->tracks.size() != 1 || collect_sink->frames.empty()) {
        cerr << "transcode sink did not forward track/frame" << endl;
        return 1;
    }
    if (collect_sink->frames.front()->getCodecId() != CodecJPEG) {
        cerr << "unexpected sink codec: " << getCodecName(collect_sink->frames.front()->getCodecId()) << endl;
        return 1;
    }
    cout << "sink frames: " << collect_sink->frames.size() << endl;

    class FakePlayer : public PlayerBase {
    public:
        explicit FakePlayer(Track::Ptr track) {
            tracks.emplace_back(std::move(track));
        }

        vector<Track::Ptr> getTracks(bool ready = true) const override {
            return tracks;
        }

        void setMediaSource(const MediaSource::Ptr &src) override {}
        void setOnShutdown(const Event &cb) override {}
        void setOnPlayResult(const Event &cb) override {}
        void setOnResume(const std::function<void()> &cb) override {}

    protected:
        void onResume() override {}
        void onShutdown(const toolkit::SockException &ex) override {}
        void onPlayResult(const toolkit::SockException &ex) override {}

    public:
        vector<Track::Ptr> tracks;
    };

    ProtocolOption option;
    option.enable_rtsp = false;
    option.enable_rtmp = false;
    option.enable_hls = false;
    option.enable_hls_fmp4 = false;
    option.enable_mp4 = false;
    option.enable_ts = false;
    option.enable_fmp4 = false;
    MediaTuple tuple = {"__defaultVhost__", "live", "transcode_proxy_test", ""};
    auto proxy = std::make_shared<PlayerProxy>(tuple, option);
    proxy->_delegate = std::make_shared<FakePlayer>(input_track);
    (*proxy)["in_process_transcode"] = "1";
    (*proxy)["transcode.codec"] = "JPEG";
    (*proxy)["transcode.width"] = "32";
    (*proxy)["transcode.height"] = "32";
    proxy->onPlaySuccess();
    if (!proxy->_video_proxy_sink) {
        cerr << "proxy player did not enable in-process transcode sink" << endl;
        return 1;
    }
    cout << "proxy transcode sink enabled" << endl;
    return 0;
#else
    cout << "ENABLE_FFMPEG off, skip" << endl;
    return 0;
#endif
}
