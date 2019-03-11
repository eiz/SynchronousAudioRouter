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

#pragma once

#include "targetver.h"

#include <windows.h>
#include <CommCtrl.h>
#include <prsht.h>
#include <windowsx.h>
#include <ShlObj.h>
#include <Shlwapi.h>
#include <SetupAPI.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <Psapi.h>

#define _ATL_CSTRING_EXPLICIT_CONSTRUCTORS

#include <atlbase.h>
#include <atlcom.h>
#include <atlstr.h>

#include <atomic>
#include <codecvt>
#include <cstddef>
#include <cstdint>
#include <locale>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <mutex>

#include "resource.h"

#define GOOGLE_GLOG_DLL_DECL
#define GLOG_NO_ABBREVIATED_SEVERITIES
#include <glog/logging.h>

