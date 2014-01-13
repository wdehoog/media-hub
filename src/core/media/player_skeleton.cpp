/*
 * Copyright © 2013 Canonical Ltd.
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

#include <core/media/property.h>

#include "player_skeleton.h"
#include "player_traits.h"
#include "property_stub.h"
#include "the_session_bus.h"

#include "mpris/player.h"

#include <org/freedesktop/dbus/stub.h>

namespace dbus = org::freedesktop::dbus;
namespace media = core::ubuntu::media;

struct media::PlayerSkeleton::Private
{
    Private(media::PlayerSkeleton* player, const dbus::types::ObjectPath& session)
        : impl(player),
          object(impl->access_service()->add_object_for_path(session)),
          properties
          {
              object->get_property<mpris::Player::Properties::CanPlay>(),
              object->get_property<mpris::Player::Properties::CanPause>(),
              object->get_property<mpris::Player::Properties::CanSeek>(),
              object->get_property<mpris::Player::Properties::CanControl>(),
              object->get_property<mpris::Player::Properties::CanGoNext>(),
              object->get_property<mpris::Player::Properties::CanGoPrevious>(),
              object->get_property<mpris::Player::Properties::PlaybackStatus>(),
              object->get_property<mpris::Player::Properties::LoopStatus>(),
              object->get_property<mpris::Player::Properties::PlaybackRate>(),
              object->get_property<mpris::Player::Properties::Shuffle>(),
              object->get_property<mpris::Player::Properties::MetaData>(),
              object->get_property<mpris::Player::Properties::Volume>(),
              object->get_property<mpris::Player::Properties::MinimumRate>(),
              object->get_property<mpris::Player::Properties::MaximumRate>()
          }
    {
    }

    void handle_next(DBusMessage* msg)
    {
        impl->next();
        auto reply = dbus::Message::make_method_return(msg);
        impl->access_bus()->send(reply->get());
    }

    void handle_previous(DBusMessage* msg)
    {
        impl->previous();
        auto reply = dbus::Message::make_method_return(msg);
        impl->access_bus()->send(reply->get());
    }

    void handle_pause(DBusMessage* msg)
    {
        impl->pause();
        auto reply = dbus::Message::make_method_return(msg);
        impl->access_bus()->send(reply->get());
    }

    void handle_playpause(DBusMessage*)
    {
    }

    void handle_stop(DBusMessage* msg)
    {
        impl->stop();
        auto reply = dbus::Message::make_method_return(msg);
        impl->access_bus()->send(reply->get());
    }

    void handle_play(DBusMessage* msg)
    {
        impl->play();
        auto reply = dbus::Message::make_method_return(msg);
        impl->access_bus()->send(reply->get());
    }

    void handle_seek(DBusMessage* msg)
    {
        auto in = dbus::Message::from_raw_message(msg);
        uint64_t ticks;
        in->reader() >> ticks; 
        impl->seek_to(std::chrono::microseconds(ticks));
    }

    void handle_set_position(DBusMessage*)
    {
    }

    void handle_open_uri(DBusMessage* msg)
    {
        auto in = dbus::Message::from_raw_message(msg);
        Track::UriType uri;
        in->reader() >> uri;

        auto reply = dbus::Message::make_method_return(msg);
        reply->writer() << impl->open_uri(uri);
        impl->access_bus()->send(reply->get());
    }

    media::PlayerSkeleton* impl;
    dbus::Object::Ptr object;
    struct
    {
        PropertyStub<bool, mpris::Player::Properties::CanPlay> can_play;
        PropertyStub<bool, mpris::Player::Properties::CanPause> can_pause;
        PropertyStub<bool, mpris::Player::Properties::CanSeek> can_seek;
        PropertyStub<bool, mpris::Player::Properties::CanControl> can_control;
        PropertyStub<bool, mpris::Player::Properties::CanGoNext> can_go_next;
        PropertyStub<bool, mpris::Player::Properties::CanGoPrevious> can_go_previous;
        
        PropertyStub<Player::PlaybackStatus, mpris::Player::Properties::PlaybackStatus> playback_status;
        PropertyStub<Player::LoopStatus, mpris::Player::Properties::LoopStatus> loop_status;
        PropertyStub<Player::PlaybackRate, mpris::Player::Properties::PlaybackRate> playback_rate;
        PropertyStub<bool, mpris::Player::Properties::Shuffle> is_shuffle;
        PropertyStub<Track::MetaData, mpris::Player::Properties::MetaData> meta_data_for_current_track;
        PropertyStub<Player::Volume, mpris::Player::Properties::Volume> volume;
        PropertyStub<Player::PlaybackRate, mpris::Player::Properties::MinimumRate> minimum_playback_rate;
        PropertyStub<Player::PlaybackRate, mpris::Player::Properties::MaximumRate> maximum_playback_rate;        
    } properties;

    /*struct
    {
        std::shared_ptr<dbus::Signal<mpris::Player::Signals::Seeked, uint64_t>> seeked;
        } signals;*/

};

media::PlayerSkeleton::PlayerSkeleton(
    const org::freedesktop::dbus::types::ObjectPath& session_path)
        : dbus::Skeleton<media::Player>(the_session_bus()),
          d(new Private{this, session_path})
{
    d->object->install_method_handler<mpris::Player::Next>(
        std::bind(&Private::handle_next,
                  std::ref(d),
                  std::placeholders::_1));
    d->object->install_method_handler<mpris::Player::Previous>(
        std::bind(&Private::handle_previous,
                  std::ref(d),
                  std::placeholders::_1));
    d->object->install_method_handler<mpris::Player::Pause>(
        std::bind(&Private::handle_pause,
                  std::ref(d),
                  std::placeholders::_1));
    d->object->install_method_handler<mpris::Player::Stop>(
        std::bind(&Private::handle_stop,
                  std::ref(d),
                  std::placeholders::_1));
    d->object->install_method_handler<mpris::Player::Play>(
        std::bind(&Private::handle_play,
                  std::ref(d),
                  std::placeholders::_1));
    d->object->install_method_handler<mpris::Player::Seek>(
        std::bind(&Private::handle_seek,
                  std::ref(d),
                  std::placeholders::_1));
    d->object->install_method_handler<mpris::Player::SetPosition>(
        std::bind(&Private::handle_set_position,
                  std::ref(d),
                  std::placeholders::_1));
    d->object->install_method_handler<mpris::Player::OpenUri>(
        std::bind(&Private::handle_open_uri,
                  std::ref(d),
                  std::placeholders::_1));
}

media::PlayerSkeleton::~PlayerSkeleton()
{
}

const media::Property<bool>& media::PlayerSkeleton::can_play() const 
{
    return d->properties.can_play;
}

const media::Property<bool>& media::PlayerSkeleton::can_pause() const
{
    return d->properties.can_pause;
}

const media::Property<bool>& media::PlayerSkeleton::can_seek() const
{
    return d->properties.can_seek;
}

const media::Property<bool>& media::PlayerSkeleton::can_go_previous() const
{
    return d->properties.can_go_previous;
}

const media::Property<bool>& media::PlayerSkeleton::can_go_next() const
{
    return d->properties.can_go_next;
}

const media::Property<media::Player::PlaybackStatus>& media::PlayerSkeleton::playback_status() const
{
    return d->properties.playback_status;
}

const media::Property<media::Player::LoopStatus>& media::PlayerSkeleton::loop_status() const
{
    return d->properties.loop_status;
}

const media::Property<media::Player::PlaybackRate>& media::PlayerSkeleton::playback_rate() const
{
    return d->properties.playback_rate;
}

const media::Property<bool>& media::PlayerSkeleton::is_shuffle() const
{
    return d->properties.is_shuffle;
}

const media::Property<media::Track::MetaData>& media::PlayerSkeleton::meta_data_for_current_track() const
{
    return d->properties.meta_data_for_current_track;
}

const media::Property<media::Player::Volume>& media::PlayerSkeleton::volume() const
{
    return d->properties.volume;
}

const media::Property<media::Player::PlaybackRate>& media::PlayerSkeleton::minimum_playback_rate() const
{
    return d->properties.minimum_playback_rate;
}

const media::Property<media::Player::PlaybackRate>& media::PlayerSkeleton::maximum_playback_rate() const
{
    return d->properties.maximum_playback_rate;
}

media::Property<media::Player::LoopStatus>& media::PlayerSkeleton::loop_status()
{
    return d->properties.loop_status;
}

media::Property<media::Player::PlaybackRate>& media::PlayerSkeleton::playback_rate()
{
    return d->properties.playback_rate;
}

media::Property<bool>& media::PlayerSkeleton::is_shuffle()
{
    return d->properties.is_shuffle;
}

media::Property<media::Player::Volume>& media::PlayerSkeleton::volume()
{
    return d->properties.volume;
}

media::Property<media::Player::PlaybackStatus>& media::PlayerSkeleton::playback_status()
{
    return d->properties.playback_status;
}

media::Property<bool>& media::PlayerSkeleton::can_play()
{
    return d->properties.can_play;
}

media::Property<bool>& media::PlayerSkeleton::can_pause()
{
    return d->properties.can_pause;
}

media::Property<bool>& media::PlayerSkeleton::can_seek()
{
    return d->properties.can_seek;
}

media::Property<bool>& media::PlayerSkeleton::can_go_previous()
{
    return d->properties.can_go_previous;
}

media::Property<bool>& media::PlayerSkeleton::can_go_next()
{
    return d->properties.can_go_next;
}

media::Property<media::Track::MetaData>& media::PlayerSkeleton::meta_data_for_current_track()
{
    return d->properties.meta_data_for_current_track;
}

media::Property<media::Player::PlaybackRate>& media::PlayerSkeleton::minimum_playback_rate()
{
    return d->properties.minimum_playback_rate;
}

media::Property<media::Player::PlaybackRate>& media::PlayerSkeleton::maximum_playback_rate()
{
    return d->properties.maximum_playback_rate;
}

const media::Signal<uint64_t>& media::PlayerSkeleton::seeked_to() const
{
    static const media::Signal<uint64_t> signal;
    return signal;
}
