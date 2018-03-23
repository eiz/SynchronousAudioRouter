// SynchronousAudioRouter
// Copyright (C) 2015 Mackenzie Straight
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with SynchronousAudioRouter.  If not, see <http://www.gnu.org/licenses/>.

#ifndef _SAR_ASIO_UTILITY_H
#define _SAR_ASIO_UTILITY_H

namespace Sar {

#ifndef UNICODE
#error "SarAsio must be built with unicode"
#endif

std::string TCHARToUTF8(const TCHAR *ptr);
std::string ConfigurationPath(const std::string& name);
std::string LoggingPath();
std::wstring UTF8ToWide(const std::string& str);

struct RunningApplication
{
    std::string name;
    std::string path;
};

std::vector<RunningApplication> RunningApplications();

} // namespace Sar
#endif // _SAR_ASIO_UTILITY_H
