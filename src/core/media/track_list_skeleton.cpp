/*
 * Copyright © 2015 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY {} without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Thomas Voß <thomas.voss@canonical.com>
 */

#include "track_list_skeleton.h"
#include "track_list_implementation.h"

#include <core/media/player.h>
#include <core/media/track_list.h>

#include "codec.h"
#include "property_stub.h"
#include "track_list_traits.h"
#include "the_session_bus.h"

#include "mpris/player.h"
#include "mpris/track_list.h"

#include "util/uri_check.h"
#include "core/media/logger/logger.h"

#include <core/dbus/object.h>
#include <core/dbus/property.h>
#include <core/dbus/types/object_path.h>
#include <core/dbus/types/variant.h>
#include <core/dbus/types/stl/map.h>
#include <core/dbus/types/stl/vector.h>

#include <iostream>
#include <limits>
#include <cstdint>

namespace dbus = core::dbus;
namespace media = core::ubuntu::media;

using namespace std;

struct media::TrackListSkeleton::Private
{
    Private(media::TrackListSkeleton* impl, const dbus::Bus::Ptr& bus, const dbus::Object::Ptr& object,
            const apparmor::ubuntu::RequestContextResolver::Ptr& request_context_resolver,
            const media::apparmor::ubuntu::RequestAuthenticator::Ptr& request_authenticator)
        : impl(impl),
          bus(bus),
          object(object),
          request_context_resolver(request_context_resolver),
          request_authenticator(request_authenticator),
          uri_check(std::make_shared<UriCheck>()),
          skeleton(mpris::TrackList::Skeleton::Configuration{object, mpris::TrackList::Skeleton::Configuration::Defaults{}}),
          current_track(skeleton.properties.tracks->get().begin()),
          empty_iterator(skeleton.properties.tracks->get().begin()),
          loop_status(media::Player::LoopStatus::none),
          current_position(0),
          id_after_remove(),
          signals
          {
              skeleton.signals.track_added,
              skeleton.signals.tracks_added,
              skeleton.signals.track_moved,
              skeleton.signals.track_removed,
              skeleton.signals.track_changed,
              skeleton.signals.track_list_reset,
              skeleton.signals.tracklist_replaced
          }
    {
    }

    void handle_get_tracks_metadata(const core::dbus::Message::Ptr& msg)
    {
        media::Track::Id track;
        msg->reader() >> track;

        const auto meta_data = impl->query_meta_data_for_track(track);

        const auto reply = dbus::Message::make_method_return(msg);
        reply->writer() << *meta_data;
        bus->send(reply);
    }

    void handle_get_tracks_uri(const core::dbus::Message::Ptr& msg)
    {
        media::Track::Id track;
        msg->reader() >> track;

        const auto uri = impl->query_uri_for_track(track);

        const auto reply = dbus::Message::make_method_return(msg);
        reply->writer() << uri;
        bus->send(reply);
    }

    void handle_add_track_with_uri_at(const core::dbus::Message::Ptr& msg)
    {
        MH_TRACE("");
        request_context_resolver->resolve_context_for_dbus_name_async
            (msg->sender(), [this, msg](const media::apparmor::ubuntu::Context& context)
        {
            Track::UriType uri;
            media::Track::Id after;
            bool make_current;
            msg->reader() >> uri >> after >> make_current;

            // Make sure the client has adequate apparmor permissions to open the URI
            const auto result = request_authenticator->authenticate_open_uri_request(context, uri);
            auto reply = dbus::Message::make_method_return(msg);

            uri_check->set(uri);
            const bool valid_uri = !uri_check->is_local_file() or
                    (uri_check->is_local_file() and uri_check->file_exists());
            if (!valid_uri)
            {
                const std::string err_str = {"Warning: Not adding track " + uri +
                     " to TrackList because it can't be found."};
                MH_WARNING("%s", err_str.c_str());
                reply = dbus::Message::make_error(
                            msg,
                            mpris::Player::Error::UriNotFound::name,
                            err_str);
            }
            else
            {
                // Only add the track to the TrackList if it passes the apparmor permissions check
                if (std::get<0>(result))
                {
                    impl->add_track_with_uri_at(uri, after, make_current);
                }
                else
                {
                    const std::string err_str = {"Warning: Not adding track " + uri +
                        " to TrackList because of inadequate client apparmor permissions."};
                    MH_WARNING("%s", err_str.c_str());
                    reply = dbus::Message::make_error(
                                msg,
                                mpris::TrackList::Error::InsufficientPermissionsToAddTrack::name,
                                err_str);
                }
            }

            bus->send(reply);
        });
    }

    void handle_add_tracks_with_uri_at(const core::dbus::Message::Ptr& msg)
    {
        MH_TRACE("");
        request_context_resolver->resolve_context_for_dbus_name_async
            (msg->sender(), [this, msg](const media::apparmor::ubuntu::Context& context)
        {
            ContainerURI uris;
            media::Track::Id after;
            msg->reader() >> uris >> after;

            bool valid_uri = false;
            media::apparmor::ubuntu::RequestAuthenticator::Result result;
            std::string uri_err_str, err_str;
            core::dbus::Message::Ptr reply;
            for (const auto uri : uris)
            {
                uri_check->set(uri);
                valid_uri = !uri_check->is_local_file() or
                        (uri_check->is_local_file() and uri_check->file_exists());
                if (!valid_uri)
                {
                    uri_err_str = {"Warning: Not adding track " + uri +
                         " to TrackList because it can't be found."};
                    MH_WARNING("%s", uri_err_str.c_str());
                    reply = dbus::Message::make_error(
                                msg,
                                mpris::Player::Error::UriNotFound::name,
                                err_str);
                }

                // Make sure the client has adequate apparmor permissions to open the URI
                result = request_authenticator->authenticate_open_uri_request(context, uri);
                if (not std::get<0>(result))
                {
                    err_str = {"Warning: Not adding track " + uri +
                        " to TrackList because of inadequate client apparmor permissions."};
                    break;
                }
            }

            // Only add the track to the TrackList if it passes the apparmor permissions check
            if (std::get<0>(result))
            {
                reply = dbus::Message::make_method_return(msg);
                impl->add_tracks_with_uri_at(uris, after);
            }
            else
            {
                MH_WARNING("%s", err_str.c_str());
                reply = dbus::Message::make_error(
                            msg,
                            mpris::TrackList::Error::InsufficientPermissionsToAddTrack::name,
                            err_str);
            }

            bus->send(reply);
        });
    }

    void handle_move_track(const core::dbus::Message::Ptr& msg)
    {
        media::Track::Id id;
        media::Track::Id to;
        msg->reader() >> id >> to;

        core::dbus::Message::Ptr reply;
        try {
            const bool ret = impl->move_track(id, to);
            if (!ret)
            {
                const std::string err_str = {"Error: Not moving track " + id +
                    " to destination " + to};
                MH_WARNING("%s", err_str.c_str());
                reply = dbus::Message::make_error(
                        msg,
                        mpris::TrackList::Error::FailedToMoveTrack::name,
                        err_str);
            }
            else
            {
                reply = dbus::Message::make_method_return(msg);
            }
        } catch(media::TrackList::Errors::FailedToMoveTrack& e) {
            reply = dbus::Message::make_error(
                    msg,
                    mpris::TrackList::Error::FailedToFindMoveTrackSource::name,
                    e.what());
        } catch(media::TrackList::Errors::FailedToFindMoveTrackSource& e) {
            reply = dbus::Message::make_error(
                    msg,
                    mpris::TrackList::Error::FailedToFindMoveTrackSource::name,
                    e.what());
        } catch(media::TrackList::Errors::FailedToFindMoveTrackDest& e) {
            reply = dbus::Message::make_error(
                    msg,
                    mpris::TrackList::Error::FailedToFindMoveTrackDest::name,
                    e.what());
        }

        bus->send(reply);
    }

    void handle_remove_track(const core::dbus::Message::Ptr& msg)
    {
        media::Track::Id track;
        msg->reader() >> track;

        auto id_it = find(impl->tracks().get().begin(), impl->tracks().get().end(), track);
        if (id_it == impl->tracks().get().end()) {
            stringstream err_str;
            err_str << "Track " << track << " not found in track list";
            MH_WARNING("%s", err_str.str());
            auto reply = dbus::Message::make_error(
                            msg,
                            mpris::TrackList::Error::TrackNotFound::name,
                            err_str.str());
            bus->send(reply);
            return;
        }

        media::Track::Id next;
        bool deleting_current = false;

        if (id_it == impl->current_iterator())
        {
            MH_DEBUG("Removing current track");
            deleting_current = true;

            if (current_track != empty_iterator)
            {
                ++current_track;

                if (current_track == impl->tracks().get().end()
                            && loop_status == media::Player::LoopStatus::playlist)
                {
                    // Removed the last track, current is the first track and make sure that
                    // the player starts playing it
                    current_track = impl->tracks().get().begin();
                }

                if (current_track == impl->tracks().get().end())
                {
                    current_track = empty_iterator;
                    // Nothing else to play, stop playback
                    impl->emit_on_end_of_tracklist();
                }
                else
                {
                    next = *current_track;
                }
            }
        }
        else if (current_track != empty_iterator)
        {
            next = *current_track;
        }
        id_after_remove = next;

        // Calls reset_current_iterator_if_needed(), which updates the iterator
        impl->remove_track(track);

        if ((not next.empty()) and deleting_current)
            impl->go_to(next);

        auto reply = dbus::Message::make_method_return(msg);
        bus->send(reply);
    }

    void handle_go_to(const core::dbus::Message::Ptr& msg)
    {
        media::Track::Id track;
        msg->reader() >> track;

        current_track = std::find(skeleton.properties.tracks->get().begin(), skeleton.properties.tracks->get().end(), track);
        impl->go_to(track);

        auto reply = dbus::Message::make_method_return(msg);
        bus->send(reply);
    }

    void handle_reset(const core::dbus::Message::Ptr& msg)
    {
        impl->reset();

        auto reply = dbus::Message::make_method_return(msg);
        bus->send(reply);
    }

    media::TrackListSkeleton* impl;
    dbus::Bus::Ptr bus;
    dbus::Object::Ptr object;
    media::apparmor::ubuntu::RequestContextResolver::Ptr request_context_resolver;
    media::apparmor::ubuntu::RequestAuthenticator::Ptr request_authenticator;
    media::UriCheck::Ptr uri_check;

    mpris::TrackList::Skeleton skeleton;
    TrackList::ConstIterator current_track;
    TrackList::ConstIterator empty_iterator;
    media::Player::LoopStatus loop_status;
    uint64_t current_position;
    media::Track::Id id_after_remove;

    struct Signals
    {
        typedef core::dbus::Signal<mpris::TrackList::Signals::TrackAdded, mpris::TrackList::Signals::TrackAdded::ArgumentType> DBusTrackAddedSignal;
        typedef core::dbus::Signal<mpris::TrackList::Signals::TracksAdded, mpris::TrackList::Signals::TracksAdded::ArgumentType> DBusTracksAddedSignal;
        typedef core::dbus::Signal<mpris::TrackList::Signals::TrackMoved, mpris::TrackList::Signals::TrackMoved::ArgumentType> DBusTrackMovedSignal;
        typedef core::dbus::Signal<mpris::TrackList::Signals::TrackRemoved, mpris::TrackList::Signals::TrackRemoved::ArgumentType> DBusTrackRemovedSignal;
        typedef core::dbus::Signal<mpris::TrackList::Signals::TrackChanged, mpris::TrackList::Signals::TrackChanged::ArgumentType> DBusTrackChangedSignal;
        typedef core::dbus::Signal<
            mpris::TrackList::Signals::TrackListReset,
            mpris::TrackList::Signals::TrackListReset::ArgumentType>
                DBusTrackListResetSignal;
        typedef core::dbus::Signal<mpris::TrackList::Signals::TrackListReplaced, mpris::TrackList::Signals::TrackListReplaced::ArgumentType> DBusTrackListReplacedSignal;

        Signals(const std::shared_ptr<DBusTrackAddedSignal>& remote_track_added,
                const std::shared_ptr<DBusTracksAddedSignal>& remote_tracks_added,
                const std::shared_ptr<DBusTrackMovedSignal>& remote_track_moved,
                const std::shared_ptr<DBusTrackRemovedSignal>& remote_track_removed,
                const std::shared_ptr<DBusTrackChangedSignal>& remote_track_changed,
                const std::shared_ptr<DBusTrackListResetSignal>& remote_track_list_reset,
                const std::shared_ptr<DBusTrackListReplacedSignal>& remote_track_list_replaced)
        {
            // Connect all of the MPRIS interface signals to be emitted over dbus
            on_track_added.connect([remote_track_added](const media::Track::Id &id)
            {
                remote_track_added->emit(id);
            });

            on_tracks_added.connect([remote_tracks_added](const media::TrackList::ContainerURI &tracks)
            {
                remote_tracks_added->emit(tracks);
            });

            on_track_moved.connect([remote_track_moved](const media::TrackList::TrackIdTuple &ids)
            {
                remote_track_moved->emit(ids);
            });

            on_track_removed.connect([remote_track_removed](const media::Track::Id &id)
            {
                remote_track_removed->emit(id);
            });

            on_track_list_reset.connect([remote_track_list_reset]()
            {
                remote_track_list_reset->emit();
            });

            on_track_changed.connect([remote_track_changed](const media::Track::Id &id)
            {
                remote_track_changed->emit(id);
            });

            on_track_list_replaced.connect([remote_track_list_replaced](const media::TrackList::ContainerTrackIdTuple &tltuple)
            {
                remote_track_list_replaced->emit(tltuple);
            });
        }

        core::Signal<Track::Id> on_track_added;
        core::Signal<TrackList::ContainerURI> on_tracks_added;
        core::Signal<TrackList::TrackIdTuple> on_track_moved;
        core::Signal<Track::Id> on_track_removed;
        core::Signal<void> on_track_list_reset;
        core::Signal<Track::Id> on_track_changed;
        core::Signal<TrackList::ContainerTrackIdTuple> on_track_list_replaced;
        core::Signal<Track::Id> on_go_to_track;
        core::Signal<void> on_end_of_tracklist;
    } signals;
};

media::TrackListSkeleton::TrackListSkeleton(const core::dbus::Bus::Ptr& bus, const core::dbus::Object::Ptr& object,
        const media::apparmor::ubuntu::RequestContextResolver::Ptr& request_context_resolver,
        const media::apparmor::ubuntu::RequestAuthenticator::Ptr& request_authenticator)
    : d(new Private(this, bus, object, request_context_resolver, request_authenticator))
{
    d->object->install_method_handler<mpris::TrackList::GetTracksMetadata>(
        std::bind(&Private::handle_get_tracks_metadata,
                  std::ref(d),
                  std::placeholders::_1));

    d->object->install_method_handler<mpris::TrackList::GetTracksUri>(
        std::bind(&Private::handle_get_tracks_uri,
                  std::ref(d),
                  std::placeholders::_1));

    d->object->install_method_handler<mpris::TrackList::AddTrack>(
        std::bind(&Private::handle_add_track_with_uri_at,
                  std::ref(d),
                  std::placeholders::_1));

    d->object->install_method_handler<mpris::TrackList::AddTracks>(
        std::bind(&Private::handle_add_tracks_with_uri_at,
                  std::ref(d),
                  std::placeholders::_1));

    d->object->install_method_handler<mpris::TrackList::MoveTrack>(
        std::bind(&Private::handle_move_track,
                  std::ref(d),
                  std::placeholders::_1));

    d->object->install_method_handler<mpris::TrackList::RemoveTrack>(
        std::bind(&Private::handle_remove_track,
                  std::ref(d),
                  std::placeholders::_1));

    d->object->install_method_handler<mpris::TrackList::GoTo>(
        std::bind(&Private::handle_go_to,
                  std::ref(d),
                  std::placeholders::_1));

    d->object->install_method_handler<mpris::TrackList::Reset>(
        std::bind(&Private::handle_reset,
                  std::ref(d),
                  std::placeholders::_1));
}

media::TrackListSkeleton::~TrackListSkeleton()
{
}

/*
 * NOTE We do not consider the loop status in this function due to the use of it
 * we do in TrackListSkeleton::next() (the function is used to know whether we
 * need to wrap when looping is active).
 */
bool media::TrackListSkeleton::has_next()
{
    const auto n_tracks = tracks().get().size();

    if (n_tracks == 0)
        return false;

    // TODO Using current_iterator() makes media-hub crash later. Logic for
    // handling the iterators must be reviewed. As a minimum updates to the
    // track list should update current_track instead of the list being sneakly
    // changed in player_implementation.cpp.
    // To avoid the crash we consider that current_track will be eventually
    // initialized to the first track when current_iterator() gets called.
    if (d->current_track == d->empty_iterator)
    {
        if (n_tracks < 2)
            return false;
        else
            return true;
    }

    if (shuffle())
    {
        auto it = get_current_shuffled();
        return ++it != shuffled_tracks().end();
    }
    else
    {
        const auto next_track = std::next(current_iterator());
        return !is_last_track(next_track);
    }
}

/*
 * NOTE We do not consider the loop status in this function due to the use of it
 * we do in TrackListSkeleton::previous() (the function is used to know whether we
 * need to wrap when looping is active).
 */
bool media::TrackListSkeleton::has_previous()
{
    if (tracks().get().empty() || d->current_track == d->empty_iterator)
        return false;

    if (shuffle())
        return get_current_shuffled() != shuffled_tracks().begin();
    else
        return d->current_track != std::begin(tracks().get());
}

media::TrackList::ConstIterator media::TrackListSkeleton::get_current_shuffled()
{
    auto current_id = *current_iterator();
    return find(shuffled_tracks().begin(), shuffled_tracks().end(), current_id);
}

media::Track::Id media::TrackListSkeleton::next()
{
    MH_TRACE("");
    if (tracks().get().empty()) {
        // TODO Change ServiceSkeleton to return with error from DBus call
        MH_ERROR("No tracks, cannot go to next");
        return media::Track::Id{};
    }

    bool go_to_track = false;

    // End of the track reached so loop around to the beginning of the track
    if (d->loop_status == media::Player::LoopStatus::track)
    {
        MH_INFO("Looping on the current track since LoopStatus is set to track");
        go_to_track = true;
    }
    // End of the tracklist reached so loop around to the beginning of the tracklist
    else if (d->loop_status == media::Player::LoopStatus::playlist && not has_next())
    {
        MH_INFO("Looping on the tracklist since LoopStatus is set to playlist");

        if (shuffle())
        {
            const auto id = *shuffled_tracks().begin();
            set_current_track(id);
        }
        else
        {
            d->current_track = tracks().get().begin();
        }
        go_to_track = true;
    }
    else
    {
        if (shuffle())
        {
            auto it = get_current_shuffled();
            if (++it != shuffled_tracks().end()) {
                MH_INFO("Advancing to next track: %s", *it);
                set_current_track(*it);
                go_to_track = true;
            }
        }
        else
        {
            const auto it = std::next(current_iterator());
            if (not is_last_track(it))
            {
                MH_INFO("Advancing to next track: %s", *it);
                d->current_track = it;
                go_to_track = true;
            }
        }

    }

    if (go_to_track)
    {
        MH_DEBUG("next track id is %s", *(current_iterator()));
        on_track_changed()(*(current_iterator()));
        const media::Track::Id id = *(current_iterator());
        // Signal the PlayerImplementation to play the next track
        on_go_to_track()(id);
    }
    else
    {
        // At the end of the tracklist and not set to loop
        MH_INFO("End of tracklist reached");
        on_end_of_tracklist()();
    }

    return *(current_iterator());
}

media::Track::Id media::TrackListSkeleton::previous()
{
    MH_TRACE("");
    if (tracks().get().empty()) {
        // TODO Change ServiceSkeleton to return with error from DBus call
        MH_ERROR("No tracks, cannot go to previous");
        return media::Track::Id{};
    }

    bool go_to_track = false;
    // Position is measured in nanoseconds
    const uint64_t max_position = 5 * UINT64_C(1000000000);

    // If we're playing the current track for > max_position time then
    // repeat it from the beginning
    if (d->current_position > max_position)
    {
        MH_INFO("Repeating current track...");
        go_to_track = true;
    }
    // Loop on the current track forever
    else if (d->loop_status == media::Player::LoopStatus::track)
    {
        MH_INFO("Looping on the current track...");
        go_to_track = true;
    }
    // Loop over the whole playlist and repeat
    else if (d->loop_status == media::Player::LoopStatus::playlist && not has_previous())
    {
        MH_INFO("Looping on the entire TrackList...");

        if (shuffle())
        {
            const auto id = *std::prev(shuffled_tracks().end());
            set_current_track(id);
        }
        else
        {
            d->current_track = std::prev(tracks().get().end());
        }

        go_to_track = true;
    }
    else
    {
        if (shuffle())
        {
            auto it = get_current_shuffled();
            if (it != shuffled_tracks().begin()) {
                set_current_track(*(--it));
                go_to_track = true;
            }
        }
        else if (not is_first_track(current_iterator()))
        {
            // Keep returning the previous track until the first track is reached
            d->current_track = std::prev(current_iterator());
            go_to_track = true;
        }
    }

    if (go_to_track)
    {
        on_track_changed()(*(current_iterator()));
        const media::Track::Id id = *(current_iterator());
        on_go_to_track()(id);
    }
    else
    {
        // At the beginning of the tracklist and not set to loop
        MH_INFO("Beginning of tracklist reached");
        on_end_of_tracklist()();
    }

    return *(current_iterator());
}

const media::Track::Id& media::TrackListSkeleton::current()
{
    return *(current_iterator());
}

const media::TrackList::ConstIterator& media::TrackListSkeleton::current_iterator()
{
    // Prevent the TrackList from sitting at the end which will cause
    // a segfault when calling current()
    if (tracks().get().size() && (d->current_track == d->empty_iterator))
    {
        MH_DEBUG("Wrapping d->current_track back to begin()");
        d->current_track = d->skeleton.properties.tracks->get().begin();
    }
    else if (tracks().get().empty())
    {
        MH_ERROR("TrackList is empty therefore there is no valid current track");
    }

    return d->current_track;
}

bool media::TrackListSkeleton::update_current_iterator(const TrackList::ConstIterator &it)
{
    MH_TRACE("");
    if (it == tracks().get().end())
        return false;

    d->current_track = it;

    return true;
}

void media::TrackListSkeleton::reset_current_iterator_if_needed()
{
    d->current_track = find(tracks().get().begin(), tracks().get().end(), d->id_after_remove);
    if (d->current_track == tracks().get().end())
        d->current_track = d->empty_iterator;
}

media::Track::Id media::TrackListSkeleton::get_current_track(void)
{
    if (d->current_track == d->empty_iterator || tracks().get().empty())
        return media::Track::Id{};

    return *(current_iterator());
}

void media::TrackListSkeleton::set_current_track(const media::Track::Id& id)
{
    const auto id_it = find(tracks().get().begin(), tracks().get().end(), id);
    if (id_it != tracks().get().end())
        d->current_track = id_it;
}

void media::TrackListSkeleton::emit_on_end_of_tracklist()
{
    on_end_of_tracklist()();
}

const core::Property<bool>& media::TrackListSkeleton::can_edit_tracks() const
{
    return *d->skeleton.properties.can_edit_tracks;
}

core::Property<bool>& media::TrackListSkeleton::can_edit_tracks()
{
    return *d->skeleton.properties.can_edit_tracks;
}

core::Property<media::TrackList::Container>& media::TrackListSkeleton::tracks()
{
    return *d->skeleton.properties.tracks;
}

void media::TrackListSkeleton::on_position_changed(uint64_t position)
{
    d->current_position = position;
}

void media::TrackListSkeleton::on_loop_status_changed(const media::Player::LoopStatus& loop_status)
{
    d->loop_status = loop_status;
}

media::Player::LoopStatus media::TrackListSkeleton::loop_status() const
{
    return d->loop_status;
}

void media::TrackListSkeleton::on_shuffle_changed(bool shuffle)
{
    MH_TRACE("");
    set_shuffle(shuffle);
}

const core::Property<media::TrackList::Container>& media::TrackListSkeleton::tracks() const
{
    return *d->skeleton.properties.tracks;
}

const core::Signal<media::TrackList::ContainerTrackIdTuple>& media::TrackListSkeleton::on_track_list_replaced() const
{
    // Print the TrackList instance
    MH_DEBUG("%s", *this);
    return d->signals.on_track_list_replaced;
}

const core::Signal<media::Track::Id>& media::TrackListSkeleton::on_track_added() const
{
    return d->signals.on_track_added;
}

const core::Signal<media::TrackList::ContainerURI>& media::TrackListSkeleton::on_tracks_added() const
{
    return d->signals.on_tracks_added;
}

const core::Signal<media::TrackList::TrackIdTuple>& media::TrackListSkeleton::on_track_moved() const
{
    return d->signals.on_track_moved;
}

const core::Signal<media::Track::Id>& media::TrackListSkeleton::on_track_removed() const
{
    return d->signals.on_track_removed;
}

const core::Signal<void>& media::TrackListSkeleton::on_track_list_reset() const
{
    return d->signals.on_track_list_reset;
}

const core::Signal<media::Track::Id>& media::TrackListSkeleton::on_track_changed() const
{
    return d->signals.on_track_changed;
}

const core::Signal<media::Track::Id>& media::TrackListSkeleton::on_go_to_track() const
{
    return d->signals.on_go_to_track;
}

const core::Signal<void>& media::TrackListSkeleton::on_end_of_tracklist() const
{
    return d->signals.on_end_of_tracklist;
}

core::Signal<media::TrackList::ContainerTrackIdTuple>& media::TrackListSkeleton::on_track_list_replaced()
{
    return d->signals.on_track_list_replaced;
}

core::Signal<media::Track::Id>& media::TrackListSkeleton::on_track_added()
{
    return d->signals.on_track_added;
}

core::Signal<media::TrackList::ContainerURI>& media::TrackListSkeleton::on_tracks_added()
{
    return d->signals.on_tracks_added;
}

core::Signal<media::TrackList::TrackIdTuple>& media::TrackListSkeleton::on_track_moved()
{
    return d->signals.on_track_moved;
}

core::Signal<media::Track::Id>& media::TrackListSkeleton::on_track_removed()
{
    return d->signals.on_track_removed;
}

core::Signal<void>& media::TrackListSkeleton::on_track_list_reset()
{
    return d->signals.on_track_list_reset;
}

core::Signal<media::Track::Id>& media::TrackListSkeleton::on_track_changed()
{
    return d->signals.on_track_changed;
}

core::Signal<media::Track::Id>& media::TrackListSkeleton::on_go_to_track()
{
    return d->signals.on_go_to_track;
}

core::Signal<void>& media::TrackListSkeleton::on_end_of_tracklist()
{
    return d->signals.on_end_of_tracklist;
}

void media::TrackListSkeleton::reset()
{
    d->current_track = d->empty_iterator;
}

// operator<< pretty prints the given TrackList to the given output stream.
inline std::ostream& media::operator<<(std::ostream& out, const media::TrackList& tracklist)
{
    auto non_const_tl = const_cast<media::TrackList*>(&tracklist);
    out << "TrackList\n---------------" << std::endl;
    for (const media::Track::Id &id : tracklist.tracks().get())
    {
        // '*' denotes the current track
        out << "\t" << ((dynamic_cast<media::TrackListSkeleton*>(non_const_tl)->current() == id) ? "*" : "");
        out << "Track Id: " << id << std::endl;
        out << "\t\turi: " << dynamic_cast<media::TrackListImplementation*>(non_const_tl)->query_uri_for_track(id) << std::endl;
    }

    out << "---------------\nEnd TrackList" << std::endl;
    return out;
}

