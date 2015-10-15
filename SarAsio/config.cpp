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

#include <fstream>

namespace Sar {

bool EndpointConfig::load(picojson::object& obj)
{
    auto poId = obj.find("id");
    auto poDescription = obj.find("description");
    auto poType = obj.find("type");
    auto poChannelCount = obj.find("channelCount");

    if (poId == obj.end() || poDescription == obj.end() ||
        poType == obj.end() || poChannelCount == obj.end()) {
        return false;
    }

    if (!poId->second.is<std::string>() ||
        !poDescription->second.is<std::string>() ||
        !poType->second.is<std::string>() ||
        !poChannelCount->second.is<double>()) {
        return false;
    }

    auto strType = poType->second.get<std::string>();

    if (strType == "recording") {
        type = EndpointType::Recording;
    } else {
        type = EndpointType::Playback;
    }

    id = poId->second.get<std::string>();
    description = poDescription->second.get<std::string>();
    channelCount = (int)poChannelCount->second.get<double>();
    return true;
}

picojson::object EndpointConfig::save()
{
    picojson::object result;

    result.insert(std::make_pair("id", picojson::value(id)));
    result.insert(std::make_pair("description", picojson::value(description)));
    result.insert(std::make_pair("type", picojson::value(
        type == EndpointType::Recording ? "recording" : "playback")));
    result.insert(std::make_pair("channelCount",
        picojson::value(double(channelCount))));
    return result;
}

bool DefaultEndpointConfig::load(picojson::object& obj)
{
    auto poRole = obj.find("role");
    auto poType = obj.find("type");
    auto poId = obj.find("id");

    if (poRole == obj.end() || poType == obj.end() || poId == obj.end()) {
        return false;
    }

    if (!poRole->second.is<std::string>() ||
        !poType->second.is<std::string>() ||
        !poId->second.is<std::string>()) {

        return false;
    }

    auto roleStr = poRole->second.get<std::string>();
    auto typeStr = poType->second.get<std::string>();

    id = poId->second.get<std::string>();

    if (roleStr == "console") {
        role = ERole::eConsole;
    } else if (roleStr == "communications") {
        role = ERole::eCommunications;
    } else if (roleStr == "media") {
        role = ERole::eMultimedia;
    } else {
        return false;
    }

    if (typeStr == "playback") {
        type = EDataFlow::eRender;
    } else if (typeStr == "recording") {
        type = EDataFlow::eCapture;
    } else {
        return false;
    }

    return true;
}

picojson::object DefaultEndpointConfig::save()
{
    picojson::object result;
    std::string roleStr;
    std::string typeStr;

    result.insert(std::make_pair("id", picojson::value(id)));

    switch (role) {
        case ERole::eConsole: roleStr = "console"; break;
        case ERole::eCommunications: roleStr = "communications"; break;
        case ERole::eMultimedia: roleStr = "multimedia"; break;
    }

    switch (type) {
        case EDataFlow::eRender: typeStr = "playback"; break;
        case EDataFlow::eCapture: typeStr = "recording"; break;
    }

    result.insert(std::make_pair("role", picojson::value(roleStr)));
    result.insert(std::make_pair("type", picojson::value(typeStr)));
    return result;
}

bool ApplicationConfig::load(picojson::object& obj)
{
    auto poPath = obj.find("path");
    auto poDefaults = obj.find("defaults");

    if (poPath == obj.end()) {
        return false;
    }

    if (!poPath->second.is<std::string>()) {
        return false;
    }

    if (poDefaults != obj.end() && poDefaults->second.is<picojson::array>()) {
        for (auto& item : poDefaults->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }

            DefaultEndpointConfig defaultEndpoint;

            if (defaultEndpoint.load(item.get<picojson::object>())) {
                defaults.push_back(defaultEndpoint);
            }
        }
    }

    try {
        path = poPath->second.get<std::string>();
        // TODO: UTF-8 support on Windows is very sad. This might need ICU or
        // PCRE.
        pattern = std::regex(path,
            std::regex_constants::ECMAScript | std::regex_constants::icase);
    } catch (std::exception&) {
        // nom nom
    }

    return true;
}

picojson::object ApplicationConfig::save()
{
    picojson::object result;

    result.insert(std::make_pair("path", picojson::value(path)));

    if (defaults.size()) {
        picojson::array arr;

        for (auto& defaultEndpoint : defaults) {
            arr.push_back(picojson::value(defaultEndpoint.save()));
        }

        result.insert(std::make_pair("defaults", picojson::value(arr)));
    }

    return result;
}

void DriverConfig::load(picojson::object& obj)
{
    auto poDriverClsid = obj.find("driverClsid");
    auto poEndpoints = obj.find("endpoints");
    auto poApplications = obj.find("applications");
    auto poWaveRtMinimumFrames = obj.find("waveRtMinimumFrames");

    if (poDriverClsid != obj.end() &&
        poDriverClsid->second.is<std::string>()) {

        driverClsid = poDriverClsid->second.get<std::string>();
    }

    if (poEndpoints != obj.end() && poEndpoints->second.is<picojson::array>()) {
        for (auto& item : poEndpoints->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }

            EndpointConfig endpoint;

            if (endpoint.load(item.get<picojson::object>())) {
                endpoints.emplace_back(endpoint);
            }
        }
    }

    if (poApplications != obj.end() &&
        poApplications->second.is<picojson::array>()) {
        for (auto& item : poApplications->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }

            ApplicationConfig application;

            if (application.load(item.get<picojson::object>())) {
                applications.emplace_back(application);
            }
        }
    }

    if (poWaveRtMinimumFrames != obj.end() &&
        poWaveRtMinimumFrames->second.is<double>()) {

        waveRtMinimumFrames = (int)poWaveRtMinimumFrames->second.get<double>();
    }
}

picojson::object DriverConfig::save()
{
    picojson::object result;

    result.insert(std::make_pair("driverClsid", picojson::value(driverClsid)));

    if (endpoints.size()) {
        picojson::array arr;

        for (auto& endpoint : endpoints) {
            arr.emplace_back(endpoint.save());
        }

        result.insert(std::make_pair("endpoints", picojson::value(arr)));
    }

    if (applications.size()) {
        picojson::array arr;

        for (auto& application : applications) {
            arr.emplace_back(application.save());
        }

        result.insert(std::make_pair("applications", picojson::value(arr)));
    }

    return result;
}

bool DriverConfig::writeFile(const std::string& path)
{
    std::ofstream fp(path);

    if (fp.bad()) {
        return false;
    }

    picojson::value(save()).serialize(std::ostream_iterator<char>(fp), true);
    return true;
}

DriverConfig DriverConfig::fromFile(const std::string& path)
{
    std::ifstream fp(path);
    picojson::value json;
    DriverConfig result;

    if (fp.bad()) {
        return result;
    }

    fp >> json;

    if (json.is<picojson::object>()) {
        result.load(json.get<picojson::object>());
    }

    return result;
}

EndpointConfig *DriverConfig::findEndpoint(const std::string& id)
{
    for (auto& ep : endpoints) {
        if (ep.id == id) {
            return &ep;
        }
    }

    return nullptr;
}

} // namespace Sar
