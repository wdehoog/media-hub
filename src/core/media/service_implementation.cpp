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
 *              Jim Hodapp <jim.hodapp@canonical.com>
 *              Ricardo Mendoza <ricardo.mendoza@canonical.com>
 *
 * Note: Some of the PulseAudio code was adapted from telepathy-ofono
 */

#include "service_implementation.h"

#include "apparmor/ubuntu.h"
#include "audio/output_observer.h"
#include "client_death_observer.h"
#include "player_configuration.h"
#include "player_skeleton.h"
#include "player_implementation.h"
#include "power/battery_observer.h"
#include "power/state_controller.h"
#include "recorder_observer.h"
#include "telephony/call_monitor.h"

#include <boost/asio.hpp>

#include <string>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <thread>
#include <utility>

#include <pulse/pulseaudio.h>

#include "util/timeout.h"

namespace media = core::ubuntu::media;

using namespace std;

struct media::ServiceImplementation::Private
{
    // Create all of the appropriate observers and helper class instances to be
    // passed to the PlayerImplementation
    Private(const ServiceImplementation::Configuration& configuration)
        : configuration(configuration),
          resume_key(std::numeric_limits<std::uint32_t>::max()),
          battery_observer(media::power::make_platform_default_battery_observer(configuration.external_services)),
          power_state_controller(media::power::make_platform_default_state_controller(configuration.external_services)),
          display_state_lock(power_state_controller->display_state_lock()),
          client_death_observer(media::platform_default_client_death_observer()),
          recorder_observer(media::make_platform_default_recorder_observer()),
          audio_output_observer(media::audio::make_platform_default_output_observer()),
          request_context_resolver(media::apparmor::ubuntu::make_platform_default_request_context_resolver(configuration.external_services)),
          request_authenticator(media::apparmor::ubuntu::make_platform_default_request_authenticator()),
          audio_output_state(media::audio::OutputState::Speaker),
          call_monitor(media::telephony::make_platform_default_call_monitor())
    {
    }

    media::ServiceImplementation::Configuration configuration;
    // This holds the key of the multimedia role Player instance that was paused
    // when the battery level reached 10% or 5%
    media::Player::PlayerKey resume_key;
    media::power::BatteryObserver::Ptr battery_observer;
    media::power::StateController::Ptr power_state_controller;
    media::power::StateController::Lock<media::power::DisplayState>::Ptr display_state_lock;
    media::ClientDeathObserver::Ptr client_death_observer;
    media::RecorderObserver::Ptr recorder_observer;
    media::audio::OutputObserver::Ptr audio_output_observer;
    media::apparmor::ubuntu::RequestContextResolver::Ptr request_context_resolver;
    media::apparmor::ubuntu::RequestAuthenticator::Ptr request_authenticator;
    media::audio::OutputState audio_output_state;

    media::telephony::CallMonitor::Ptr call_monitor;
    // Holds a pair of a Player key denoting what player to resume playback, and a bool
    // for if it should be resumed after a phone call is hung up
    std::list<std::pair<media::Player::PlayerKey, bool>> paused_sessions;
};

media::ServiceImplementation::ServiceImplementation(const Configuration& configuration)
    : d(new Private(configuration))
{
    d->battery_observer->level().changed().connect([this](const media::power::Level& level)
    {
        const bool resume_play_after_phonecall = false;
        // When the battery level hits 10% or 5%, pause all multimedia sessions.
        // Playback will resume when the user clears the presented notification.
        switch (level)
        {
        case media::power::Level::low:
        case media::power::Level::very_low:
            // Whatever player session is currently playing, make sure it is NOT resumed after
            // a phonecall is hung up
            pause_all_multimedia_sessions(resume_play_after_phonecall);
            break;
        default:
            break;
        }
    });

    d->battery_observer->is_warning_active().changed().connect([this](bool active)
    {
        // If the low battery level notification is no longer being displayed,
        // resume what the user was previously playing
        if (!active)
            resume_multimedia_session();
    });

    d->audio_output_observer->external_output_state().changed().connect([this](audio::OutputState state)
    {
        const bool resume_play_after_phonecall = false;
        switch (state)
        {
        case audio::OutputState::Earpiece:
            std::cout << "AudioOutputObserver reports that output is now Headphones/Headset." << std::endl;
            break;
        case audio::OutputState::Speaker:
            std::cout << "AudioOutputObserver reports that output is now Speaker." << std::endl;
            // Whatever player session is currently playing, make sure it is NOT resumed after
            // a phonecall is hung up
            pause_all_multimedia_sessions(resume_play_after_phonecall);
            break;
        case audio::OutputState::External:
            std::cout << "AudioOutputObserver reports that output is now External." << std::endl;
            break;
        }
        d->audio_output_state = state;
    });

    d->call_monitor->on_call_state_changed().connect([this](media::telephony::CallMonitor::State state)
    {
        const bool resume_play_after_phonecall = true;
        switch (state) {
        case media::telephony::CallMonitor::State::OffHook:
            std::cout << "Got call started signal, pausing all multimedia sessions" << std::endl;
            // Whatever player session is currently playing, make sure it gets resumed after
            // a phonecall is hung up
            pause_all_multimedia_sessions(resume_play_after_phonecall);
            break;
        case media::telephony::CallMonitor::State::OnHook:
            std::cout << "Got call ended signal, resuming paused multimedia sessions" << std::endl;
            resume_paused_multimedia_sessions(false);
            break;
        }
    });

    d->recorder_observer->recording_state().changed().connect([this](RecordingState state)
    {
        if (state == media::RecordingState::started)
        {
            d->display_state_lock->request_acquire(media::power::DisplayState::on);
            // Whatever player session is currently playing, make sure it is NOT resumed after
            // a phonecall is hung up
            const bool resume_play_after_phonecall = false;
            pause_all_multimedia_sessions(resume_play_after_phonecall);
        }
        else if (state == media::RecordingState::stopped)
        {
            d->display_state_lock->request_release(media::power::DisplayState::on);
        }
    });
}

media::ServiceImplementation::~ServiceImplementation()
{
}

std::shared_ptr<media::Player> media::ServiceImplementation::create_session(
        const media::Player::Configuration& conf)
{
    auto player = std::make_shared<media::PlayerImplementation<media::PlayerSkeleton>>(media::PlayerImplementation<media::PlayerSkeleton>::Configuration
    {
        media::PlayerSkeleton::Configuration
        {
            conf.bus,
            conf.service,
            conf.session,
            d->request_context_resolver,
            d->request_authenticator
        },
        conf.key,
        d->client_death_observer,
        d->power_state_controller
    });

    auto key = conf.key;
    player->on_client_disconnected().connect([this, key]()
    {
        // Call remove_player_for_key asynchronously otherwise deadlock can occur
        // if called within this dispatcher context.
        // remove_player_for_key can destroy the player instance which in turn
        // destroys the "on_client_disconnected" signal whose destructor will wait
        // until all dispatches are done
        d->configuration.external_services.io_service.post([this, key]()
        {
            if (!d->configuration.player_store->has_player_for_key(key))
                return;

            if (d->configuration.player_store->player_for_key(key)->lifetime() == Player::Lifetime::normal)
                d->configuration.player_store->remove_player_for_key(key);
        });
    });

    return player;
}

std::shared_ptr<media::Player> media::ServiceImplementation::create_fixed_session(const std::string&, const media::Player::Configuration&)
{
  // no impl
  return std::shared_ptr<media::Player>();
}

std::shared_ptr<media::Player> media::ServiceImplementation::resume_session(media::Player::PlayerKey)
{
  // no impl
  return std::shared_ptr<media::Player>();
}

void media::ServiceImplementation::pause_other_sessions(media::Player::PlayerKey key)
{
    if (not d->configuration.player_store->has_player_for_key(key))
    {
        cerr << "Could not find Player by key: " << key << endl;
        return;
    }

    auto current_player = d->configuration.player_store->player_for_key(key);

    // We immediately make the player known as new current player.
    if (current_player->audio_stream_role() == media::Player::multimedia)
        d->configuration.player_store->set_current_player_for_key(key);

    d->configuration.player_store->enumerate_players([current_player, key](const media::Player::PlayerKey& other_key, const std::shared_ptr<media::Player>& other_player)
    {
        // Only pause a Player if all of the following criteria are met:
        // 1) currently playing
        // 2) not the same player as the one passed in my key
        // 3) new Player has an audio stream role set to multimedia
        // 4) has an audio stream role set to multimedia
        if (other_player->playback_status() == Player::playing &&
            other_key != key &&
            current_player->audio_stream_role() == media::Player::multimedia &&
            other_player->audio_stream_role() == media::Player::multimedia)
        {
            cout << "Pausing Player with key: " << other_key << endl;
            other_player->pause();
        }
    });
}

void media::ServiceImplementation::pause_all_multimedia_sessions(bool resume_play_after_phonecall)
{
    d->configuration.player_store->enumerate_players([this, resume_play_after_phonecall](const media::Player::PlayerKey& key, const std::shared_ptr<media::Player>& player)
    {
        if (player->playback_status() == Player::playing
            && player->audio_stream_role() == media::Player::multimedia)
        {
            auto paused_player_pair = std::make_pair(key, resume_play_after_phonecall);
            d->paused_sessions.push_back(paused_player_pair);
            std::cout << "Pausing Player with key: " << key << ", resuming after phone call? "
                << (resume_play_after_phonecall ? "yes" : "no") << std::endl;
            player->pause();
        }
    });
}

void media::ServiceImplementation::resume_paused_multimedia_sessions(bool resume_video_sessions)
{
    std::for_each(d->paused_sessions.begin(), d->paused_sessions.end(),
        [this, resume_video_sessions](const std::pair<media::Player::PlayerKey, bool> &paused_player_pair) {
            const media::Player::PlayerKey key = paused_player_pair.first;
            const bool resume_play_after_phonecall = paused_player_pair.second;
            auto player = d->configuration.player_store->player_for_key(key);
            // Only resume video playback if explicitly desired
            if ((resume_video_sessions || player->is_audio_source()) && resume_play_after_phonecall)
                player->play();
            else
                std::cout << "Not auto-resuming video player session or other type of player session." << std::endl;
        });

    d->paused_sessions.clear();
}

void media::ServiceImplementation::resume_multimedia_session()
{
    if (not d->configuration.player_store->has_player_for_key(d->resume_key))
        return;

    auto player = d->configuration.player_store->player_for_key(d->resume_key);

    if (player->playback_status() == Player::paused)
    {
        cout << "Resuming playback of Player with key: " << d->resume_key << endl;
        player->play();
        d->resume_key = std::numeric_limits<std::uint32_t>::max();
    }
}
