// SynchronousAudioRouter
// Copyright (C) 2015 Mackenzie Straight
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SynchronousAudioRouter.  If not, see <http://www.gnu.org/licenses/>.

#include "stdafx.h"
#include "config.h"

namespace Sar {

bool EndpointConfig::load(picojson::object& obj)
{
    auto poId = obj.find("id");
    auto poDescription = obj.find("description");

    if (poId == obj.end() || poDescription == obj.end()) {
        return false;
    }

    if (!poId->second.is<std::string>() ||
        !poDescription->second.is<std::string>()) {
        return false;
    }

    id = poId->second.get<std::string>();
    description = poDescription->second.get<std::string>();
    return true;
}

bool ApplicationConfig::load(picojson::object& obj)
{
    return true;
}

void DriverConfig::load(picojson::object& obj)
{

}

DriverConfig DriverConfig::fromFile(const std::string& path)
{
    DriverConfig result;

    return result;
}


} // namespace Sar
