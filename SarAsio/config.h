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
    Playback,
    Recording
};

struct EndpointConfig
{
    std::string id;
    std::string description;
    EndpointType type;
    int channelCount = 2;

    bool load(picojson::object& obj);
    picojson::object save();
};

struct DefaultEndpointConfig
{
    EDataFlow type = EDataFlow::eRender;
    ERole role = ERole::eMultimedia;
    std::string id;

    bool load(picojson::object& obj);
    picojson::object save();
};

struct ApplicationConfig
{
    std::string path;
    std::regex pattern;
    std::vector<DefaultEndpointConfig> defaults;

    bool load(picojson::object& obj);
    picojson::object save();
};

struct DriverConfig
{
    std::string driverClsid;
    std::vector<EndpointConfig> endpoints;
    std::vector<ApplicationConfig> applications;
    int waveRtMinimumFrames = 0;
    bool enableApplicationRouting = false;

    void load(picojson::object& obj);
    picojson::object save();
    bool writeFile(const std::string& path);
    static DriverConfig fromFile(const std::string& path);
    EndpointConfig *findEndpoint(const std::string& id);
};

} // namespace Sar
#endif // _SAR_ASIO_CONFIG_H
