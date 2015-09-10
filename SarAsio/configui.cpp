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
#include "configui.h"
#include "dllmain.h"

using namespace Sar;

PropertySheetPage::PropertySheetPage()
{
    ZeroMemory(static_cast<PROPSHEETPAGE *>(this), sizeof(PROPSHEETPAGE));
    dwSize = sizeof(PROPSHEETPAGE);
    hInstance = gDllModule;
    pfnDlgProc = &dialogProcStub;
    lParam = (LPARAM)this;
}

HPROPSHEETPAGE PropertySheetPage::create()
{
    return nullptr;
}

INT_PTR CALLBACK PropertySheetPage::dialogProcStub(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_INITDIALOG) {
        PropertySheetPage *page =
            (PropertySheetPage *)((PROPSHEETPAGE *)lparam)->lParam;

        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)page);
    }

    PropertySheetPage *page =
        (PropertySheetPage *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (page) {
        return page->dialogProc(hwnd, msg, wparam, lparam);
    }

    return 0;
}

PropertyDialog::PropertyDialog()
{
    ZeroMemory(static_cast<PROPSHEETHEADER *>(this), sizeof(PROPSHEETHEADER));
    dwSize = sizeof(PROPSHEETHEADER);
    dwFlags = PSH_DEFAULT;
    hInstance = gDllModule;
}

void PropertyDialog::addPage(std::shared_ptr<PropertySheetPage> page)
{
    HPROPSHEETPAGE handle = ::CreatePropertySheetPage(&*page);

    if (handle) {
        _pages.emplace_back(page);
        _pageHandles.emplace_back(handle);
    }
}

INT_PTR PropertyDialog::show(HWND parent)
{
    nPages = (UINT)_pages.size();
    phpage = _pageHandles.data();
    hwndParent = parent;

    return ::PropertySheet(this);
}

EndpointsPropertySheetPage::EndpointsPropertySheetPage()
{
    pszTemplate = MAKEINTRESOURCE(IDD_CONFIG_ENDPOINTS);
}

INT_PTR EndpointsPropertySheetPage::dialogProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
        case WM_INITDIALOG:
            break;
    }

    return 0;
}

ApplicationsPropertySheetPage::ApplicationsPropertySheetPage()
{
    pszTemplate = MAKEINTRESOURCE(IDD_CONFIG_APPLICATIONS);
}

INT_PTR ApplicationsPropertySheetPage::dialogProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    return 0;
}

ConfigurationPropertyDialog::ConfigurationPropertyDialog()
    : endpoints(std::make_shared<EndpointsPropertySheetPage>()),
      applications(std::make_shared<ApplicationsPropertySheetPage>())
{
    pszCaption = TEXT("Synchronous Audio Router");
    addPage(endpoints);
    addPage(applications);
}
