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

#ifndef _SAR_ASIO_CONFIGUI_H
#define _SAR_ASIO_CONFIGUI_H

#include "config.h"
#include "tinyasio.h"

namespace Sar {

struct PropertySheetPage: public PROPSHEETPAGE
{
    PropertySheetPage();

protected:
    virtual INT_PTR dialogProc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) = 0;
    void changed();

    HWND _hwnd;

private:
    static INT_PTR CALLBACK dialogProcStub(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
};

struct PropertyDialog: public PROPSHEETHEADER
{
    PropertyDialog();
    void addPage(std::shared_ptr<PropertySheetPage> page);
    INT_PTR show(HWND parent = nullptr);

private:
    std::vector<std::shared_ptr<PropertySheetPage>> _pages;
    std::vector<HPROPSHEETPAGE> _pageHandles;
};

struct SimpleDialog
{
    SimpleDialog(LPCTSTR templateName);
    INT_PTR show(HWND parent = nullptr);

protected:
    virtual INT_PTR dialogProc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) = 0;

    HWND _hwnd;

private:
    static INT_PTR CALLBACK dialogProcStub(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    LPCTSTR _templateName;
};

struct EndpointsPropertySheetPage: public PropertySheetPage
{
    EndpointsPropertySheetPage(DriverConfig& config);

protected:
    virtual INT_PTR dialogProc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    void initControls();
    void refreshHardwareInterfaceList();
    void refreshEndpointList();
    void updateEnabled();
    void onHardwareInterfaceChanged();
    void onConfigureHardwareInterface();
    void onAddEndpoint();
    void onRemoveEndpoint();

    DriverConfig& _config;
    std::vector<AsioDriver> _drivers;
    HWND _hardwareInterfaceDropdown;
    HWND _hardwareInterfaceConfigButton;
    HWND _listView;
    HWND _addButton;
    HWND _removeButton;

    void initEpDialogControls();
    void updateEpDialogConfig();
    INT_PTR epDialogProc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    static INT_PTR CALLBACK epDialogProcStub(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    HWND _epDialog;
    HWND _epDialogName;
    HWND _epDialogType;
    HWND _epDialogChannelCount;
    EndpointConfig _epDialogConfig;
};

struct ApplicationsPropertySheetPage: public PropertySheetPage
{
    ApplicationsPropertySheetPage(DriverConfig& config);

protected:
    virtual INT_PTR dialogProc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    void initControls();
    void refreshApplicationList();
    void updateEnabled();
    void onAddApplication();

    DriverConfig& _config;
    HWND _enableRouting;
    HWND _listView;
    HWND _addButton;
    HWND _removeButton;
};

struct ApplicationConfigDialog: public SimpleDialog
{
    ApplicationConfigDialog(
        DriverConfig& driverConfig, ApplicationConfig config);
    const ApplicationConfig& config() { return _config; }

protected:
    virtual INT_PTR dialogProc(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    void initControls();
    void initEndpointDropdown(HWND control, EndpointType type);
    void refreshControls();
    void refreshEndpointDropdown(HWND control, EDataFlow dataFlow, ERole role);
    int indexOfEndpoint(const std::string& id);

    DriverConfig& _driverConfig;
    ApplicationConfig _config;
    HWND _path;
    HWND _useRegularExpressions;
    HWND _runningApplicationsButton;
    HWND _browseButton;
    HWND _playbackSystem;
    HWND _playbackCommunications;
    HWND _playbackMultimedia;
    HWND _recordingSystem;
    HWND _recordingCommunications;
    HWND _recordingMultimedia;
};

struct ConfigurationPropertyDialog: public PropertyDialog
{
    ConfigurationPropertyDialog(DriverConfig& config);
    DriverConfig newConfig() { return _newConfig; }

private:
    DriverConfig& _originalConfig;
    DriverConfig _newConfig;
    std::shared_ptr<EndpointsPropertySheetPage> _endpoints;
    std::shared_ptr<ApplicationsPropertySheetPage> _applications;
};

} // namespace Sar
#endif // _SAR_ASIO_CONFIGUI_H