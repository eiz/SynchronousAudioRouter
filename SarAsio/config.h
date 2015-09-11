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

#ifndef _SAR_ASIO_CONFIG_H
#define _SAR_ASIO_CONFIG_H

#include "picojson.h"

namespace Sar {

enum class EndpointType
{
    Recording,
    Playback
};

struct EndpointConfig
{
    std::string id;
    std::string description;

    bool load(picojson::object& obj);
};

struct ApplicationConfig
{
    std::string processName;
    std::string defaultEndpointId;
    bool startAutomatically;

    bool load(picojson::object& obj);
};

struct DriverConfig
{
    std::string driverClsid;
    std::vector<EndpointConfig> endpoints;
    std::vector<ApplicationConfig> applications;

    void load(picojson::object& obj);
    static DriverConfig fromFile(const std::string& path);
};

} // namespace Sar
#endif // _SAR_ASIO_CONFIG_H