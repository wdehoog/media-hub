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

#include "service_implementation.h"

namespace music = com::ubuntu::music;

struct music::ServiceImplementation::Private
{
};

music::ServiceImplementation::ServiceImplementation() : d(new Private())
{

}

music::ServiceImplementation::~ServiceImplementation()
{
}

std::shared_ptr<music::Player> music::ServiceImplementation::create_session(const music::Player::Configuration&)
{
    return std::shared_ptr<music::Player>();
}