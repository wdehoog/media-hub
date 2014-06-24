/*
 * Copyright © 2013-2014 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Voß <thomas.voss@canonical.com>
 */

#include <core/media/service.h>
#include <core/media/player.h>
#include <core/media/track_list.h>

#include "core/media/gstreamer/engine.h"

#include "../test_data.h"
#include "../waitable_state_transition.h"

#include <gtest/gtest.h>

#include <cstdio>

#include <condition_variable>
#include <functional>
#include <thread>

namespace media = core::ubuntu::media;

struct EnsureFakeAudioSinkEnvVarIsSet
{
    EnsureFakeAudioSinkEnvVarIsSet()
    {
        ::setenv("CORE_UBUNTU_MEDIA_SERVICE_AUDIO_SINK_NAME", "fakesink", 1);
    }
} ensure_fake_audio_sink_env_var_is_set;

struct EnsureFakeVideoSinkEnvVarIsSet
{
    EnsureFakeVideoSinkEnvVarIsSet()
    {
        ::setenv("CORE_UBUNTU_MEDIA_SERVICE_VIDEO_SINK_NAME", "fakesink", 1);
    }
};

struct EnsureMirVideoSinkEnvVarIsSet
{
    EnsureMirVideoSinkEnvVarIsSet()
    {
        ::setenv("CORE_UBUNTU_MEDIA_SERVICE_VIDEO_SINK_NAME", "mirsink", 1);
    }
};

TEST(GStreamerEngine, construction_and_deconstruction_works)
{
    gstreamer::Engine engine;
}

TEST(GStreamerEngine, setting_uri_and_starting_audio_only_playback_works)
{
    const std::string test_file{"/tmp/test.ogg"};
    const std::string test_file_uri{"file:///tmp/test.ogg"};
    std::remove(test_file.c_str());
    ASSERT_TRUE(test::copy_test_ogg_file_to(test_file));

    core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State> wst(
                core::ubuntu::media::Engine::State::ready);

    gstreamer::Engine engine;

    engine.track_meta_data().changed().connect(
                [](const std::tuple<media::Track::UriType, media::Track::MetaData>& md)
                {
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::album()))
                        EXPECT_EQ("Test", std::get<1>(md).get(media::Engine::Xesam::album()));
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::album_artist()))
                        EXPECT_EQ("Test", std::get<1>(md).get(media::Engine::Xesam::album_artist()));
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::artist()))
                        EXPECT_EQ("Test", std::get<1>(md).get(media::Engine::Xesam::artist()));
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::disc_number()))
                        EXPECT_EQ("42", std::get<1>(md).get(media::Engine::Xesam::disc_number()));
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::genre()))
                        EXPECT_EQ("Test", std::get<1>(md).get(media::Engine::Xesam::genre()));
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::track_number()))
                        EXPECT_EQ("42", std::get<1>(md).get(media::Engine::Xesam::track_number()));
                });

    engine.state().changed().connect(
                std::bind(
                    &core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State>::trigger,
                    std::ref(wst),
                    std::placeholders::_1));

    EXPECT_TRUE(engine.open_resource_for_uri(test_file_uri));
    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::seconds{60}));
}

TEST(GStreamerEngine, setting_uri_and_starting_video_playback_works)
{
    const std::string test_file{"/tmp/h264.avi"};
    const std::string test_file_uri{"file:///tmp/h264.avi"};
    std::remove(test_file.c_str());
    ASSERT_TRUE(test::copy_test_avi_file_to(test_file));
    // Make sure a video sink is added to the pipeline
    const EnsureFakeVideoSinkEnvVarIsSet efs;

    core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State> wst(
                core::ubuntu::media::Engine::State::ready);

    gstreamer::Engine engine;

    engine.track_meta_data().changed().connect(
                [](const std::tuple<media::Track::UriType, media::Track::MetaData>& md)
                {
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::album()))
                        EXPECT_EQ("Test series", std::get<1>(md).get(media::Engine::Xesam::album()));
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::artist()))
                        EXPECT_EQ("Canonical", std::get<1>(md).get(media::Engine::Xesam::artist()));
                    if (0 < std::get<1>(md).count(media::Engine::Xesam::genre()))
                        EXPECT_EQ("Documentary", std::get<1>(md).get(media::Engine::Xesam::genre()));
                });

    engine.state().changed().connect(
                std::bind(
                    &core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State>::trigger,
                    std::ref(wst),
                    std::placeholders::_1));

    EXPECT_TRUE(engine.open_resource_for_uri(test_file_uri));
    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));

    wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::ready,
                    std::chrono::seconds{10});
}

TEST(GStreamerEngine, stop_pause_play_seek_audio_only_works)
{
    const std::string test_file{"/tmp/test.ogg"};
    const std::string test_file_uri{"file:///tmp/test.ogg"};
    std::remove(test_file.c_str());
    ASSERT_TRUE(test::copy_test_mp3_file_to(test_file));

    core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State> wst(
                core::ubuntu::media::Engine::State::ready);

    gstreamer::Engine engine;

    engine.state().changed().connect(
                std::bind(
                    &core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State>::trigger,
                    std::ref(wst),
                    std::placeholders::_1));

    EXPECT_TRUE(engine.open_resource_for_uri(test_file_uri));
    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));
    EXPECT_TRUE(engine.stop());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::stopped,
                    std::chrono::milliseconds{4000}));
    EXPECT_TRUE(engine.pause());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::paused,
                    std::chrono::milliseconds{4000}));

    EXPECT_TRUE(engine.seek_to(std::chrono::seconds{10}));
    EXPECT_TRUE(engine.seek_to(std::chrono::seconds{0}));
    EXPECT_TRUE(engine.seek_to(std::chrono::seconds{25}));

    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));

    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::ready,
                    std::chrono::seconds{40}));
}

TEST(GStreamerEngine, stop_pause_play_seek_video_works)
{
    const std::string test_file{"/tmp/h264.avi"};
    const std::string test_file_uri{"file:///tmp/h264.avi"};
    std::remove(test_file.c_str());
    ASSERT_TRUE(test::copy_test_avi_file_to(test_file));
    // Make sure a video sink is added to the pipeline
    const EnsureFakeVideoSinkEnvVarIsSet efs;

    core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State> wst(
                core::ubuntu::media::Engine::State::ready);

    gstreamer::Engine engine;

    engine.state().changed().connect(
                std::bind(
                    &core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State>::trigger,
                    std::ref(wst),
                    std::placeholders::_1));

    EXPECT_TRUE(engine.open_resource_for_uri(test_file_uri));
    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));
    EXPECT_TRUE(engine.stop());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::stopped,
                    std::chrono::milliseconds{4000}));
    EXPECT_TRUE(engine.pause());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::paused,
                    std::chrono::milliseconds{4000}));

    EXPECT_TRUE(engine.seek_to(std::chrono::seconds{10}));
    EXPECT_TRUE(engine.seek_to(std::chrono::seconds{0}));
    EXPECT_TRUE(engine.seek_to(std::chrono::seconds{25}));

    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));

    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::ready,
                    std::chrono::seconds{40}));
}

TEST(GStreamerEngine, get_position_duration_work)
{
    const std::string test_file{"/tmp/h264.avi"};
    const std::string test_file_uri{"file:///tmp/h264.avi"};
    std::remove(test_file.c_str());
    ASSERT_TRUE(test::copy_test_ogg_file_to(test_file));

    core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State> wst(
                core::ubuntu::media::Engine::State::ready);

    gstreamer::Engine engine;

    engine.state().changed().connect(
                std::bind(
                    &core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State>::trigger,
                    std::ref(wst),
                    std::placeholders::_1));

    EXPECT_TRUE(engine.open_resource_for_uri(test_file_uri));
    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));

    EXPECT_TRUE(engine.seek_to(std::chrono::seconds{10}));
    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));

    std::cout << "position: " << engine.position() << std::endl;
    std::cout << "duration: " << engine.duration() << std::endl;

    // FIXME: This should be 10e9, but seek_to seems to be broken from within this unit test
    // and I haven't been able to figure out why
    EXPECT_TRUE(engine.position() > 1e9);

    EXPECT_TRUE(engine.duration() > 1e9);
}

TEST(GStreamerEngine, adjusting_volume_works)
{
    const std::string test_file{"/tmp/test.mp3"};
    const std::string test_file_uri{"file:///tmp/test.mp3"};
    std::remove(test_file.c_str());
    ASSERT_TRUE(test::copy_test_mp3_file_to(test_file));

    core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State> wst(
                core::ubuntu::media::Engine::State::ready);

    gstreamer::Engine engine;
    engine.state().changed().connect(
                std::bind(
                    &core::testing::WaitableStateTransition<core::ubuntu::media::Engine::State>::trigger,
                    std::ref(wst),
                    std::placeholders::_1));
    EXPECT_TRUE(engine.open_resource_for_uri(test_file_uri));
    EXPECT_TRUE(engine.play());
    EXPECT_TRUE(wst.wait_for_state_for(
                    core::ubuntu::media::Engine::State::playing,
                    std::chrono::milliseconds{4000}));

    std::thread t([&]()
    {
        for(unsigned i = 0; i < 100; i++)
        {
            for (double v = 0.; v < 1.1; v += 0.1)
            {
                try
                {
                    media::Engine::Volume volume{v};
                    engine.volume() = volume;
                    EXPECT_EQ(volume, engine.volume());
                } catch(...)
                {
                }
            }
        }
    });

    if (t.joinable())
        t.join();
}

TEST(GStreamerEngine, provides_non_null_meta_data_extractor)
{
    gstreamer::Engine engine;
    EXPECT_NE(nullptr, engine.meta_data_extractor());
}

TEST(GStreamerEngine, meta_data_extractor_provides_correct_tags)
{
    const std::string test_file{"/tmp/test.ogg"};
    const std::string test_file_uri{"file:///tmp/test.ogg"};
    std::remove(test_file.c_str());
    ASSERT_TRUE(test::copy_test_ogg_file_to(test_file));

    gstreamer::Engine engine;
    auto md = engine.meta_data_extractor()->meta_data_for_track_with_uri(test_file_uri);

    if (0 < md.count(media::Engine::Xesam::album()))
        EXPECT_EQ("Test", md.get(media::Engine::Xesam::album()));
    if (0 < md.count(media::Engine::Xesam::album_artist()))
        EXPECT_EQ("Test", md.get(media::Engine::Xesam::album_artist()));
    if (0 < md.count(media::Engine::Xesam::artist()))
        EXPECT_EQ("Test", md.get(media::Engine::Xesam::artist()));
    if (0 < md.count(media::Engine::Xesam::disc_number()))
        EXPECT_EQ("42", md.get(media::Engine::Xesam::disc_number()));
    if (0 < md.count(media::Engine::Xesam::genre()))
        EXPECT_EQ("Test", md.get(media::Engine::Xesam::genre()));
    if (0 < md.count(media::Engine::Xesam::track_number()))
        EXPECT_EQ("42", md.get(media::Engine::Xesam::track_number()));
}

