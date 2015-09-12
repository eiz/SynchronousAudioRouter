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
    auto poProcessName = obj.find("processName");
    auto poDefaultEndpointId = obj.find("defaultEndpointId");

    if (poProcessName == obj.end() || poDefaultEndpointId == obj.end()) {
        return false;
    }

    if (!poProcessName->second.is<std::string>() ||
        !poDefaultEndpointId->second.is<std::string>()) {
        return false;
    }

    processName = poProcessName->second.get<std::string>();
    defaultEndpointId = poDefaultEndpointId->second.get<std::string>();
    return true;
}

void DriverConfig::load(picojson::object& obj)
{
    auto poDriverClsid = obj.find("driverClsid");
    auto poEndpoints = obj.find("endpoints");
    auto poApplications = obj.find("applications");

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

            endpoint.load(item.get<picojson::object>());
            endpoints.emplace_back(endpoint);
        }
    }

    if (poApplications != obj.end() &&
        poApplications->second.is<picojson::array>()) {
        for (auto& item : poApplications->second.get<picojson::array>()) {
            if (!item.is<picojson::object>()) {
                continue;
            }

            ApplicationConfig application;

            application.load(item.get<picojson::object>());
            applications.emplace_back(application);
        }
    }
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

} // namespace Sar
