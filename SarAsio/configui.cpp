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
#include "utility.h"

using namespace Sar;

PropertySheetPage::PropertySheetPage()
{
    ZeroMemory(static_cast<PROPSHEETPAGE *>(this), sizeof(PROPSHEETPAGE));
    dwSize = sizeof(PROPSHEETPAGE);
    hInstance = gDllModule;
    pfnDlgProc = &dialogProcStub;
    lParam = (LPARAM)this;
}

INT_PTR CALLBACK PropertySheetPage::dialogProcStub(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_INITDIALOG) {
        PropertySheetPage *page =
            (PropertySheetPage *)((PROPSHEETPAGE *)lparam)->lParam;

        page->_hwnd = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)page);
    }

    PropertySheetPage *page =
        (PropertySheetPage *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (page) {
        return page->dialogProc(hwnd, msg, wparam, lparam);
    }

    return 0;
}

void PropertySheetPage::changed()
{
    PropSheet_Changed(GetParent(_hwnd), _hwnd);
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

SimpleDialog::SimpleDialog(LPCTSTR templateName):
    _templateName(templateName) { }

INT_PTR SimpleDialog::show(HWND parent)
{
    return DialogBoxParam(gDllModule, _templateName, parent,
        &SimpleDialog::dialogProcStub, (LPARAM)this);
}

INT_PTR CALLBACK SimpleDialog::dialogProcStub(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_INITDIALOG) {
        SimpleDialog *dlg = (SimpleDialog *)lparam;

        dlg->_hwnd = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)dlg);
    }

    SimpleDialog *dlg =
        (SimpleDialog *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (dlg) {
        return dlg->dialogProc(hwnd, msg, wparam, lparam);
    }

    return 0;
}

EndpointsPropertySheetPage::EndpointsPropertySheetPage(DriverConfig& config)
    : _config(config)
{
    pszTemplate = MAKEINTRESOURCE(IDD_CONFIG_ENDPOINTS);

    for (auto driver : InstalledAsioDrivers()) {
        // Skip our own driver.
        if (driver.clsid != CLSID_STR_SynchronousAudioRouter) {
            _drivers.emplace_back(driver);
        }
    }
}

INT_PTR EndpointsPropertySheetPage::dialogProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
        case WM_INITDIALOG:
            initControls();
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case 1001: // _hardwareInterfaceDropdown
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        onHardwareInterfaceChanged();
                    }

                    break;
                case 1002: // _hardwareInterfaceConfigButton
                    if (HIWORD(wparam) == BN_CLICKED) {
                        onConfigureHardwareInterface();
                    }

                    break;
                case 1005: // _addButton
                    if (HIWORD(wparam) == BN_CLICKED) {
                        onAddEndpoint();
                    }

                    break;
                case 1006: // _removeButton
                    if (HIWORD(wparam) == BN_CLICKED) {
                        onRemoveEndpoint();
                    }

                    break;
            }

            break;
        case WM_NOTIFY: {
            LPNMHDR nmh = (LPNMHDR)lparam;

            switch (nmh->idFrom) {
                case 1004: // _listView
                    if (nmh->code == LVN_ITEMCHANGED) {
                        updateEnabled();
                    }

                    break;
            }

            break;
        }
    }

    return 0;
}

void EndpointsPropertySheetPage::onHardwareInterfaceChanged()
{
    auto index = ComboBox_GetCurSel(_hardwareInterfaceDropdown) - 1;

    if (index >= 0 && index < (int)_drivers.size()) {
        _config.driverClsid = _drivers[index].clsid;
    } else {
        _config.driverClsid = "";
    }

    updateEnabled();
    changed();
}

void EndpointsPropertySheetPage::onConfigureHardwareInterface()
{
    for (auto& driver : _drivers) {
        if (driver.clsid == _config.driverClsid) {
            CComPtr<IASIO> asio;

            if (SUCCEEDED(driver.open(&asio))) {
                if (asio->init(_hwnd) != AsioBool::True) {
                    MessageBox(_hwnd,
                        TEXT("Failed to initialize ASIO driver."),
                        TEXT("Error"),
                        MB_OK | MB_ICONERROR);
                } else {
                    asio->controlPanel();
                }
            } else {
                MessageBox(_hwnd,
                    TEXT("Failed to open ASIO driver."),
                    TEXT("Error"),
                    MB_OK | MB_ICONERROR);
            }

            break;
        }
    }
}

void EndpointsPropertySheetPage::onAddEndpoint()
{
    _epDialogConfig = EndpointConfig();

    int counter = 1;

    do {
        std::ostringstream os;

        os << "ep_" << counter;

        if (!_config.findEndpoint(os.str())) {
            _epDialogConfig.id = os.str();
        } else {
            counter++;
        }
    } while (_epDialogConfig.id.empty());

    auto result = DialogBoxParam(
        gDllModule,
        MAKEINTRESOURCE(IDD_CONFIG_ENDPOINT_DETAILS),
        _hwnd,
        &EndpointsPropertySheetPage::epDialogProcStub,
        (LPARAM)this);

    if (result == IDOK) {
        _config.endpoints.push_back(_epDialogConfig);
        refreshEndpointList();
        updateEnabled();
        changed();
    }
}

void EndpointsPropertySheetPage::onRemoveEndpoint()
{
    auto index = ListView_GetNextItem(_listView, -1, LVNI_SELECTED);

    if (index < 0 || index >= (int)_config.endpoints.size()) {
        LOG(INFO) << "Index " << index
            << " out of bounds when removing endpoint from list";
        return;
    }

    _config.endpoints.erase(_config.endpoints.begin() + index);
    refreshEndpointList();
    updateEnabled();
    changed();
}

void EndpointsPropertySheetPage::initControls()
{
    _hardwareInterfaceDropdown = GetDlgItem(_hwnd, 1001);
    _hardwareInterfaceConfigButton = GetDlgItem(_hwnd, 1002);
    _listView = GetDlgItem(_hwnd, 1004);
    _addButton = GetDlgItem(_hwnd, 1005);
    _removeButton = GetDlgItem(_hwnd, 1006);

    LVCOLUMN col = {};

    col.mask = LVCF_TEXT;
    col.pszText = L"Type";
    ListView_InsertColumn(_listView, 0, &col);
    col.pszText = L"Channels";
    ListView_InsertColumn(_listView, 1, &col);
    col.pszText = L"Name";
    ListView_InsertColumn(_listView, 2, &col);
    ListView_SetExtendedListViewStyle(_listView, LVS_EX_FULLROWSELECT);

    refreshHardwareInterfaceList();
    refreshEndpointList();
    updateEnabled();
}

void EndpointsPropertySheetPage::refreshHardwareInterfaceList()
{
    ComboBox_ResetContent(_hardwareInterfaceDropdown);
    ComboBox_AddString(_hardwareInterfaceDropdown, TEXT("None"));

    int index = 1, selectIndex = 0;

    for (auto& driver : _drivers) {
        auto wstr = UTF8ToWide(driver.name);

        ComboBox_AddString(_hardwareInterfaceDropdown, wstr.c_str());

        if (driver.clsid == _config.driverClsid) {
            selectIndex = index;
        }

        index++;
    }

    ComboBox_SetCurSel(_hardwareInterfaceDropdown, selectIndex);
}

void EndpointsPropertySheetPage::refreshEndpointList()
{
    ListView_DeleteAllItems(_listView);

    int i = 0;

    for (auto& endpoint : _config.endpoints) {
        LVITEM item = {};
        std::wstring description = UTF8ToWide(endpoint.description);

        item.iItem = i;
        ListView_InsertItem(_listView, &item);
        ListView_SetItemText(_listView, i, 2, (LPWSTR)description.c_str());

        if (endpoint.type == EndpointType::Recording) {
            ListView_SetItemText(_listView, i, 0, L"Recording");
        } else {
            ListView_SetItemText(_listView, i, 0, L"Playback");
        }

        std::wostringstream wos;
        std::wstring channelCount;

        wos << endpoint.channelCount;
        channelCount = wos.str();
        ListView_SetItemText(_listView, i, 1, (LPWSTR)channelCount.c_str());
        i++;
    }

    ListView_SetColumnWidth(_listView, 0, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(_listView, 1, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(_listView, 2, LVSCW_AUTOSIZE_USEHEADER);
}

void EndpointsPropertySheetPage::updateEnabled()
{
    Button_Enable(_hardwareInterfaceConfigButton,
        ComboBox_GetCurSel(_hardwareInterfaceDropdown) > 0);
    Button_Enable(_removeButton,
        ListView_GetSelectedCount(_listView) > 0);
}

void EndpointsPropertySheetPage::initEpDialogControls()
{
    _epDialogName = GetDlgItem(_epDialog, 1101);
    _epDialogType = GetDlgItem(_epDialog, 1103);
    _epDialogChannelCount = GetDlgItem(_epDialog, 1105);

    ComboBox_AddString(_epDialogType, L"Playback");
    ComboBox_AddString(_epDialogType, L"Recording");
    ComboBox_SetCurSel(_epDialogType,
        _epDialogConfig.type == EndpointType::Recording ? 1 : 0);

    std::wostringstream wos;

    wos << _epDialogConfig.channelCount;

    Edit_SetText(_epDialogChannelCount, wos.str().c_str());
}

void EndpointsPropertySheetPage::updateEpDialogConfig()
{
    auto len = Edit_GetTextLength(_epDialogName);
    auto buf = new WCHAR[len + 1];

    Edit_GetText(_epDialogName, buf, len + 1);
    _epDialogConfig.description = TCHARToUTF8(buf);
    delete[] buf;

    len = Edit_GetTextLength(_epDialogChannelCount);
    buf = new WCHAR[len + 1];
    Edit_GetText(_epDialogChannelCount, buf, len + 1);

    auto countStr = TCHARToUTF8(buf);

    delete[] buf;
    _epDialogConfig.channelCount = max(1, std::stoi(countStr));
    _epDialogConfig.type = ComboBox_GetCurSel(_epDialogType) == 1 ?
        EndpointType::Recording : EndpointType::Playback;
}

INT_PTR EndpointsPropertySheetPage::epDialogProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    LOG(INFO) << "EndpointsPropertySheetPage::addEndpointDialogProc";

    switch (msg) {
        case WM_INITDIALOG:
            initEpDialogControls();
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case IDOK:
                    updateEpDialogConfig();

                    // fall-through
                case IDCANCEL:
                    EndDialog(hwnd, LOWORD(wparam));
                    break;
            }

            break;
    }

    return 0;
}

INT_PTR CALLBACK EndpointsPropertySheetPage::epDialogProcStub(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_INITDIALOG) {
        EndpointsPropertySheetPage *page = (EndpointsPropertySheetPage *)lparam;

        page->_epDialog = hwnd;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)page);
    }

    EndpointsPropertySheetPage *page =
        (EndpointsPropertySheetPage *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    if (page) {
        return page->epDialogProc(hwnd, msg, wparam, lparam);
    }

    return 0;
}

ApplicationsPropertySheetPage::ApplicationsPropertySheetPage(
    DriverConfig& config): _config(config)
{
    pszTemplate = MAKEINTRESOURCE(IDD_CONFIG_APPLICATIONS);
}

INT_PTR ApplicationsPropertySheetPage::dialogProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
        case WM_INITDIALOG:
            initControls();
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case 1200: // _enableRouting
                    _config.enableApplicationRouting =
                        Button_GetCheck(_enableRouting) == BST_CHECKED;
                    refreshControls();
                    updateEnabled();
                    changed();
                    break;
                case 1203: // _addButton
                    if (HIWORD(wparam) == BN_CLICKED) {
                        onAddApplication();
                    }

                    break;
                case 1204: // _removeButton
                    if (HIWORD(wparam) == BN_CLICKED) {
                        onRemoveApplication();
                    }

                    break;
            }

            break;
        case WM_NOTIFY: {
            LPNMHDR nmh = (LPNMHDR)lparam;

            switch (nmh->idFrom) {
                case 1202: // _listView
                    switch (nmh->code) {
                        case LVN_ITEMCHANGED: updateEnabled(); break;
                        case LVN_ITEMACTIVATE: onOpenApplication(); break;
                    }

                    break;
            }

            break;
        }
    }

    return 0;
}

void ApplicationsPropertySheetPage::initControls()
{
    _enableRouting = GetDlgItem(_hwnd, 1200);
    _listView = GetDlgItem(_hwnd, 1202);
    _addButton = GetDlgItem(_hwnd, 1203);
    _removeButton = GetDlgItem(_hwnd, 1204);

    LVCOLUMN col = {};

    col.mask = LVCF_TEXT;
    col.pszText = L"Name";
    ListView_InsertColumn(_listView, 0, &col);
    col.pszText = L"Regex?";
    ListView_InsertColumn(_listView, 1, &col);
    col.pszText = L"Path";
    ListView_InsertColumn(_listView, 2, &col);
    ListView_SetExtendedListViewStyle(_listView, LVS_EX_FULLROWSELECT);

    refreshControls();
    updateEnabled();
}

void ApplicationsPropertySheetPage::refreshControls()
{
    Button_SetCheck(_enableRouting, _config.enableApplicationRouting);
    refreshApplicationList();
}

void ApplicationsPropertySheetPage::refreshApplicationList()
{
    ListView_DeleteAllItems(_listView);

    int i = 0;

    for (auto& application : _config.applications) {
        std::wstring description = UTF8ToWide(application.description);
        std::wstring path = UTF8ToWide(application.path);
        LVITEM item = {};

        item.iItem = i;
        i++;

        ListView_InsertItem(_listView, &item);
        ListView_SetItemText(
            _listView, item.iItem, 0, (LPWSTR)description.c_str());
        ListView_SetItemText(
            _listView, item.iItem, 1, application.regexMatch ? L"Yes" : L"No");
        ListView_SetItemText(_listView, item.iItem, 2, (LPWSTR)path.c_str());
    }

    ListView_SetColumnWidth(_listView, 0, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(_listView, 1, LVSCW_AUTOSIZE_USEHEADER);
    ListView_SetColumnWidth(_listView, 2, LVSCW_AUTOSIZE_USEHEADER);
}

void ApplicationsPropertySheetPage::updateEnabled()
{
    EnableWindow(_listView, _config.enableApplicationRouting);
    Button_Enable(_addButton, _config.enableApplicationRouting);
    Button_Enable(_removeButton,
        _config.enableApplicationRouting &&
        ListView_GetSelectedCount(_listView) > 0);
}

void ApplicationsPropertySheetPage::onOpenApplication()
{
    auto index = ListView_GetNextItem(_listView, -1, LVNI_SELECTED);

    if (index < 0 || index >= (int)_config.applications.size()) {
        return;
    }

    ApplicationConfigDialog dlg(_config, _config.applications[index]);

    if (dlg.show(_hwnd) == IDOK) {
        _config.applications[index] = dlg.config();
        refreshApplicationList();
        updateEnabled();
        changed();
    }
}

void ApplicationsPropertySheetPage::onRemoveApplication()
{
    auto index = ListView_GetNextItem(_listView, -1, LVNI_SELECTED);

    if (index < 0 || index >= (int)_config.applications.size()) {
        LOG(INFO) << "Index " << index
            << " out of bounds when removing application from list";
        return;
    }

    _config.applications.erase(_config.applications.begin() + index);
    refreshApplicationList();
    updateEnabled();
    changed();
}

void ApplicationsPropertySheetPage::onAddApplication()
{
    ApplicationConfigDialog dlg(_config, ApplicationConfig());

    if (dlg.show(_hwnd) == IDOK) {
        _config.applications.push_back(dlg.config());
        refreshApplicationList();
        updateEnabled();
        changed();
    }
}

ApplicationConfigDialog::ApplicationConfigDialog(
    DriverConfig& driverConfig, ApplicationConfig config):
    SimpleDialog(MAKEINTRESOURCE(IDD_CONFIG_APPLICATION_DETAILS)),
    _driverConfig(driverConfig), _config(config)
{
}

INT_PTR ApplicationConfigDialog::dialogProc(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
        case WM_INITDIALOG:
            initControls();
            break;
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case 1303: // _runningApplicationsButton
                    onRunningApplicationsClicked();
                    break;
                case 1304: // _browseButton
                    onBrowseClicked();
                    break;
                case IDOK:
                    updateConfig();
                    // fall through
                case IDCANCEL:
                    EndDialog(_hwnd, LOWORD(wparam));
                    break;
            }

            break;
    }

    return 0;
}

void ApplicationConfigDialog::initControls()
{
    _name = GetDlgItem(_hwnd, 1308);
    _path = GetDlgItem(_hwnd, 1301);
    _useRegularExpressions = GetDlgItem(_hwnd, 1302);
    _runningApplicationsButton = GetDlgItem(_hwnd, 1303);
    _browseButton = GetDlgItem(_hwnd, 1304);
    _playbackSystem = GetDlgItem(_hwnd, 1320);
    _playbackCommunications = GetDlgItem(_hwnd, 1321);
    _playbackMultimedia = GetDlgItem(_hwnd, 1322);
    _recordingSystem = GetDlgItem(_hwnd, 1323);
    _recordingCommunications = GetDlgItem(_hwnd, 1324);
    _recordingMultimedia = GetDlgItem(_hwnd, 1325);

    initEndpointDropdown(_playbackSystem, EndpointType::Playback);
    initEndpointDropdown(_playbackCommunications, EndpointType::Playback);
    initEndpointDropdown(_playbackMultimedia, EndpointType::Playback);
    initEndpointDropdown(_recordingSystem, EndpointType::Recording);
    initEndpointDropdown(_recordingCommunications, EndpointType::Recording);
    initEndpointDropdown(_recordingMultimedia, EndpointType::Recording);
    refreshControls();
}

void ApplicationConfigDialog::initEndpointDropdown(
    HWND control, EndpointType type)
{
    ComboBox_ResetContent(control);
    ComboBox_AddString(control, _T("Use system default"));

    for (auto& endpoint : _driverConfig.endpoints) {
        if (endpoint.type != type) {
            continue;
        }

        std::wstring str = UTF8ToWide(endpoint.description);

        ComboBox_AddString(control, str.c_str());
    }
}

void ApplicationConfigDialog::refreshControls()
{
    auto description = UTF8ToWide(_config.description);
    auto path = UTF8ToWide(_config.path);

    Button_SetCheck(_useRegularExpressions, _config.regexMatch);
    Edit_SetText(_name, description.c_str());
    Edit_SetText(_path, path.c_str());

    refreshEndpointDropdown(
        _playbackSystem, EDataFlow::eRender, ERole::eConsole);
    refreshEndpointDropdown(
        _playbackCommunications, EDataFlow::eRender, ERole::eCommunications);
    refreshEndpointDropdown(
        _playbackMultimedia, EDataFlow::eRender, ERole::eMultimedia);
    refreshEndpointDropdown(
        _recordingSystem, EDataFlow::eCapture, ERole::eConsole);
    refreshEndpointDropdown(
        _recordingCommunications, EDataFlow::eCapture, ERole::eCommunications);
    refreshEndpointDropdown(
        _recordingMultimedia, EDataFlow::eCapture, ERole::eMultimedia);
}

void ApplicationConfigDialog::refreshEndpointDropdown(
    HWND control, EDataFlow dataFlow, ERole role)
{
    for (auto& defaultEndpoint : _config.defaults) {
        if (defaultEndpoint.role == role && defaultEndpoint.type == dataFlow) {
            ComboBox_SetCurSel(control, indexOfEndpoint(defaultEndpoint.id));
            return;
        }
    }

    ComboBox_SetCurSel(control, 0);
}

int ApplicationConfigDialog::indexOfEndpoint(const std::string& id)
{
    int playbackIndex = 0, recordingIndex = 0;

    for (auto& endpoint : _driverConfig.endpoints) {
        if (endpoint.type == EndpointType::Recording) {
            recordingIndex++;
        } else {
            playbackIndex++;
        }

        if (id == endpoint.id) {
            return endpoint.type == EndpointType::Recording ?
                recordingIndex : playbackIndex;
        }
    }

    return 0;
}

void ApplicationConfigDialog::updateConfig()
{
    _config.regexMatch = Button_GetCheck(_useRegularExpressions) == BST_CHECKED;

    auto len = Edit_GetTextLength(_name);
    auto buf = new WCHAR[len + 1];

    Edit_GetText(_name, buf, len + 1);
    _config.description = TCHARToUTF8(buf);
    delete[] buf;

    len = Edit_GetTextLength(_path);
    buf = new WCHAR[len + 1];
    Edit_GetText(_path, buf, len + 1);
    _config.path = TCHARToUTF8(buf);
    delete[] buf;

    _config.defaults.clear();
    updateDefaultEndpoint(
        _playbackSystem, EDataFlow::eRender, ERole::eConsole);
    updateDefaultEndpoint(
        _playbackCommunications, EDataFlow::eRender, ERole::eCommunications);
    updateDefaultEndpoint(
        _playbackMultimedia, EDataFlow::eRender, ERole::eMultimedia);
    updateDefaultEndpoint(
        _recordingSystem, EDataFlow::eCapture, ERole::eConsole);
    updateDefaultEndpoint(
        _recordingCommunications, EDataFlow::eCapture, ERole::eCommunications);
    updateDefaultEndpoint(
        _recordingMultimedia, EDataFlow::eCapture, ERole::eMultimedia);
}

void ApplicationConfigDialog::updateDefaultEndpoint(
    HWND control, EDataFlow dataFlow, ERole role)
{
    auto index = ComboBox_GetCurSel(control);

    if (index > 0 && index - 1 < (int)_driverConfig.endpoints.size()) {
        DefaultEndpointConfig defaultEndpoint;

        defaultEndpoint.type = dataFlow;
        defaultEndpoint.role = role;
        defaultEndpoint.id = _driverConfig.endpoints[index - 1].id;
        _config.defaults.push_back(defaultEndpoint);
    }
}

void ApplicationConfigDialog::onRunningApplicationsClicked()
{
    auto apps = RunningApplications();
    auto menu = CreatePopupMenu();
    int i = 1;
    POINT pt;

    if (apps.size() == 0) {
        MessageBox(_hwnd, L"Failed to build the running application list.",
            L"Error", MB_OK | MB_ICONERROR);
        return;
    }

    for (auto& app : apps) {
        AppendMenu(menu, MF_STRING, i++, UTF8ToWide(app.name).c_str());
    }

    GetCursorPos(&pt);

    int result = TrackPopupMenu(menu,
        TPM_LEFTALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, _hwnd, nullptr);

    if (result > 0) {
        result--;

        Edit_SetText(_path, UTF8ToWide(apps[result].path).c_str());
        Button_SetCheck(_useRegularExpressions, BST_UNCHECKED);
    }

    DestroyMenu(menu);
}

void ApplicationConfigDialog::onBrowseClicked()
{
    OPENFILENAME ofn = {};
    WCHAR buf[1024] = {};

    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = _hwnd;
    ofn.lpstrFilter = L"Executable files\0*.exe\0\0";
    ofn.lpstrTitle = L"Select Application";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 1024;

    if (GetOpenFileName(&ofn)) {
        Edit_SetText(_path, buf);
        Button_SetCheck(_useRegularExpressions, BST_UNCHECKED);
    }
}

ConfigurationPropertyDialog::ConfigurationPropertyDialog(DriverConfig& config)
    : _originalConfig(config),
      _newConfig(_originalConfig),
      _endpoints(std::make_shared<EndpointsPropertySheetPage>(_newConfig)),
      _applications(std::make_shared<ApplicationsPropertySheetPage>(_newConfig))
{
    pszCaption = TEXT("Synchronous Audio Router");
    addPage(_endpoints);
    addPage(_applications);
}
