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

namespace Sar {

struct PropertySheetPage: public PROPSHEETPAGE
{
    PropertySheetPage();

protected:
    virtual INT_PTR dialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        = 0;
    HPROPSHEETPAGE create();

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

struct EndpointsPropertySheetPage: public PropertySheetPage
{
    EndpointsPropertySheetPage();

protected:
    virtual INT_PTR dialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        override;
};

struct ApplicationsPropertySheetPage: public PropertySheetPage
{
    ApplicationsPropertySheetPage();

protected:
    virtual INT_PTR dialogProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        override;
};

struct ConfigurationPropertyDialog: public PropertyDialog
{
    ConfigurationPropertyDialog();

private:
    std::shared_ptr<EndpointsPropertySheetPage> endpoints;
    std::shared_ptr<ApplicationsPropertySheetPage> applications;
};

} // namespace Sar
#endif // _SAR_ASIO_CONFIGUI_H