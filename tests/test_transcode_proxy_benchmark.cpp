/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Common/MediaSource.h"
#include "Common/config.h"
#include "Player/PlayerProxy.h"
#include "Poller/EventPoller.h"
#include "Rtsp/Rtsp.h"
#include "Rtsp/RtspMediaSource.h"
#include "Util/NoticeCenter.h"
#include "Util/logger.h"

using namespace mediakit;
using namespace std;
using namespace toolkit;

namespace {

struct BenchmarkState {
    mutex lock;
    condition_variable changed;
    RtspMediaSource::Ptr source;
    string error;
    bool play_finished = false;
    bool play_ok = false;
    bool disconnected = false;
    chrono::steady_clock::time_point started = chrono::steady_clock::now();

    atomic<uint64_t> video_packets{0};
    atomic<uint64_t> video_frames{0};
    atomic<uint64_t> video_bytes{0};
    atomic<uint64_t> audio_packets{0};
    atomic<uint64_t> audio_bytes{0};
    atomic<bool> video_started{false};
    atomic<bool> measuring{false};
    uint32_t last_video_stamp = 0;
    bool have_video_stamp = false;
};

void configureProtocols(ProtocolOption &option) {
    option.enable_audio = true;
    option.enable_rtsp = true;
    option.enable_rtmp = false;
    option.enable_hls = false;
    option.enable_hls_fmp4 = false;
    option.enable_mp4 = false;
    option.enable_ts = false;
    option.enable_fmp4 = false;
    option.rtsp_demand = false;
}

} // namespace

int main(int argc, char **argv) {
#if !defined(ENABLE_FFMPEG)
    cout << "ENABLE_FFMPEG off, skip" << endl;
    return 0;
#else
    const char *url = getenv("ZLM_RTSP_URL");
    if (!url || !*url) {
        cout << "ZLM_RTSP_URL unset, skip" << endl;
        return 0;
    }

    int run_seconds = argc > 1 ? atoi(argv[1]) : 12;
    int target_fps = argc > 2 ? atoi(argv[2]) : 25;
    run_seconds = run_seconds < 3 ? 3 : run_seconds;
    target_fps = target_fps < 1 ? 1 : target_fps;

    Logger::Instance().add(make_shared<ConsoleChannel>("benchmark", LError));
    EventPollerPool::setPoolSize(2);
    mINI::Instance()[Rtsp::kDirectProxy] = 0;

    ProtocolOption option;
    configureProtocols(option);
    MediaTuple tuple{DEFAULT_VHOST, "live", "transcode_proxy_benchmark", ""};
    auto state = make_shared<BenchmarkState>();
    auto poller = EventPollerPool::Instance().getPoller();
    auto player = make_shared<PlayerProxy>(tuple, option, 0, poller);
    auto shutdown_player = [&]() {
        poller->sync([&]() {
            player.reset();
        });
        lock_guard<mutex> guard(state->lock);
        state->source.reset();
    };

    (*player)[Client::kRtpType] = Rtsp::RTP_TCP;
    (*player)["in_process_transcode"] = "1";
    (*player)["transcode.codec"] = "H264";
    (*player)["transcode.encoder"] = "libx264";
    (*player)["transcode.width"] = "1280";
    (*player)["transcode.height"] = "720";
    (*player)["transcode.fps"] = to_string(target_fps);
    (*player)["transcode.bitrate"] = "2000000";
    (*player)["transcode.gop"] = to_string(target_fps * 2);
    (*player)["transcode.threads"] = "4";
    (*player)["transcode.decode_threads"] = "2";
    (*player)["transcode.preset"] = "veryfast";
    (*player)["transcode.pixel_format"] = "yuv420p";
    (*player)["transcode.zerolatency"] = "1";
    (*player)["transcode.max_b_frames"] = "0";

    int listener_tag = 0;
    NoticeCenter::Instance().addListener(&listener_tag, Broadcast::kBroadcastMediaChanged,
                                         [state, tuple](BroadcastMediaChangedArgs) {
        const auto &source_tuple = sender.getMediaTuple();
        if (!bRegist || sender.getSchema() != RTSP_SCHEMA || source_tuple.vhost != tuple.vhost ||
            source_tuple.app != tuple.app || source_tuple.stream != tuple.stream) {
            return;
        }
        auto source = dynamic_pointer_cast<RtspMediaSource>(sender.shared_from_this());
        {
            lock_guard<mutex> guard(state->lock);
            state->source = std::move(source);
        }
        state->changed.notify_all();
    });

    player->setPlayCallbackOnce([state](const SockException &ex) {
        {
            lock_guard<mutex> guard(state->lock);
            state->play_finished = true;
            state->play_ok = !ex;
            if (ex) {
                state->error = ex.what();
            }
        }
        state->changed.notify_all();
    });
    player->setOnClose([state](const SockException &ex) {
        {
            lock_guard<mutex> guard(state->lock);
            state->disconnected = true;
            if (state->error.empty()) {
                state->error = ex.what();
            }
        }
        state->changed.notify_all();
    });
    player->setOnDisconnect([state]() {
        {
            lock_guard<mutex> guard(state->lock);
            state->disconnected = true;
        }
        state->changed.notify_all();
    });

    player->play(url);

    RtspMediaSource::Ptr source;
    string startup_error;
    {
        unique_lock<mutex> guard(state->lock);
        bool ready = state->changed.wait_for(guard, chrono::seconds(25), [state]() {
            return state->source || (state->play_finished && !state->play_ok) || state->disconnected;
        });
        if (!ready || !state->source) {
            startup_error = state->error;
        } else {
            source = state->source;
        }
    }
    if (!source) {
        cerr << "output source unavailable";
        if (!startup_error.empty()) {
            cerr << ": " << startup_error;
        }
        cerr << endl;
        NoticeCenter::Instance().delListener(&listener_tag, Broadcast::kBroadcastMediaChanged);
        shutdown_player();
        return 1;
    }

    auto startup_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - state->started).count();
    auto tracks = source->getTracks(true);
    bool valid_video = false;
    for (const auto &track : tracks) {
        if (track->getTrackType() != TrackVideo) {
            continue;
        }
        auto video = dynamic_pointer_cast<VideoTrack>(track);
        valid_video = valid_video || (video && video->getCodecId() == CodecH264 && video->getVideoWidth() == 1280 &&
                                      video->getVideoHeight() == 720);
        if (video) {
            cout << "video=" << track->getCodecName() << " " << video->getVideoWidth() << "x"
                 << video->getVideoHeight() << " declared_fps=" << video->getVideoFps() << endl;
        }
    }
    if (!valid_video || !source->getRing()) {
        cerr << "unexpected output video track" << endl;
        NoticeCenter::Instance().delListener(&listener_tag, Broadcast::kBroadcastMediaChanged);
        shutdown_player();
        source.reset();
        return 1;
    }

    auto reader_poller = EventPollerPool::Instance().getPoller();
    RtspMediaSource::RingType::RingReader::Ptr reader;
    reader_poller->sync([&]() {
        reader = source->getRing()->attach(reader_poller, false);
        reader->setReadCB([state](const RtspMediaSource::RingDataType &packets) {
            for (const auto &rtp : *packets) {
                if (rtp->type == TrackVideo) {
                    if (!state->video_started.exchange(true)) {
                        state->changed.notify_all();
                    }
                    if (!state->measuring) {
                        continue;
                    }
                    ++state->video_packets;
                    state->video_bytes += rtp->size();
                    auto stamp = rtp->getStamp();
                    if (!state->have_video_stamp || stamp != state->last_video_stamp) {
                        state->have_video_stamp = true;
                        state->last_video_stamp = stamp;
                        ++state->video_frames;
                    }
                } else if (rtp->type == TrackAudio) {
                    if (!state->measuring) {
                        continue;
                    }
                    ++state->audio_packets;
                    state->audio_bytes += rtp->size();
                }
            }
        });
    });

    bool video_ready = false;
    string video_error;
    {
        unique_lock<mutex> guard(state->lock);
        video_ready = state->changed.wait_for(guard, chrono::seconds(10), [state]() {
            return state->video_started || state->disconnected;
        });
        video_ready = video_ready && state->video_started && !state->disconnected;
        video_error = state->error;
    }
    if (!video_ready) {
        reader_poller->sync([&]() {
            reader->setReadCB(nullptr);
            reader.reset();
        });
        NoticeCenter::Instance().delListener(&listener_tag, Broadcast::kBroadcastMediaChanged);
        shutdown_player();
        source.reset();
        cerr << "video output unavailable";
        if (!video_error.empty()) {
            cerr << ": " << video_error;
        }
        cerr << endl;
        return 1;
    }

    auto measure_started = chrono::steady_clock::now();
    state->measuring = true;
    this_thread::sleep_for(chrono::seconds(run_seconds));
    state->measuring = false;
    auto measure_ended = chrono::steady_clock::now();
    reader_poller->sync([&]() {
        reader->setReadCB(nullptr);
        reader.reset();
    });
    auto measured_ms = chrono::duration_cast<chrono::milliseconds>(measure_ended - measure_started).count();

    NoticeCenter::Instance().delListener(&listener_tag, Broadcast::kBroadcastMediaChanged);
    double measured_seconds = measured_ms / 1000.0;
    double video_fps = state->video_frames.load() / measured_seconds;
    double video_mbps = state->video_bytes.load() * 8.0 / measured_seconds / 1000000.0;
    double audio_kbps = state->audio_bytes.load() * 8.0 / measured_seconds / 1000.0;

    cout << "target_fps=" << target_fps << " startup_ms=" << startup_ms << " duration_s=" << measured_seconds << endl;
    cout << "video_frames=" << state->video_frames.load() << " video_packets=" << state->video_packets.load()
         << " measured_fps=" << video_fps << " video_mbps=" << video_mbps << endl;
    cout << "audio_packets=" << state->audio_packets.load() << " audio_kbps=" << audio_kbps << endl;
    cout << "source_speed_Bps=" << source->getBytesSpeed() << endl;

    bool stable = false;
    {
        lock_guard<mutex> guard(state->lock);
        stable = !state->disconnected && state->video_frames != 0 && state->video_bytes != 0;
    }
    shutdown_player();
    source.reset();
    if (!stable) {
        cerr << "stream stability check failed" << endl;
        return 1;
    }
    return 0;
#endif
}
