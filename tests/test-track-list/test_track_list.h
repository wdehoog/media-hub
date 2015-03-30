/*
 * Copyright © 2013-2015 Canonical Ltd.
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
 * Authored by: Jim Hodapp <jim.hodapp@canonical.com>
 */

#ifndef TEST_TRACK_LIST_H_
#define TEST_TRACK_LIST_H_

#include <memory>
#include <string>

namespace core
{
namespace ubuntu
{
namespace media
{

class Player;
class Service;
class TrackList;

class TestTrackList
{
public:
    TestTrackList();

    void add_track(const std::string &uri, bool make_current = false);

    // Takes in one or two files for playback, adds it/them to the TrackList, and plays
    void test_basic_playback(const std::string &uri1, const std::string &uri2 = std::string{});

private:
    std::shared_ptr<core::ubuntu::media::Service> m_hubService;
    std::shared_ptr<core::ubuntu::media::Player> m_hubPlayerSession;
    std::shared_ptr<core::ubuntu::media::TrackList> m_hubTrackList;
};

} // media
} // ubuntu
} // core

#endif
