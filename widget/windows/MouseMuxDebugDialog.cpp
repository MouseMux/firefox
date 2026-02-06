/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MouseMuxDebugDialog.h"
#include "MouseMuxClient.h"
#include "InputFilter.h"
#include <cstdio>
#include <cstdarg>
#include <shlwapi.h>
#include <shlobj.h>
#include <commctrl.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

#define SUBCLASS_ID 1001

#define MOUSEMUX_VERSION "5.45"

namespace mozilla {
namespace widget {

MouseMuxDebugDialog* MouseMuxDebugDialog::sInstance = nullptr;

MouseMuxDebugDialog* MouseMuxDebugDialog::GetInstance() {
  if (!sInstance) {
    sInstance = new MouseMuxDebugDialog();
  }
  return sInstance;
}

void MouseMuxDebugDialog::Shutdown() {
  if (sInstance) {
    sInstance->Hide();
    delete sInstance;
    sInstance = nullptr;
  }
}

MouseMuxDebugDialog::MouseMuxDebugDialog() {}

void MouseMuxDebugDialog::SetClient(MouseMuxClient* client) {
  // Stop tracking old Firefox window if any
  if (mFirefoxHwnd && mFirefoxHwnd != (client ? client->GetWindowHwnd() : nullptr)) {
    StopTrackingFirefox();
  }

  mClient = client;

  // Get Firefox HWND from the client
  if (client) {
    mFirefoxHwnd = client->GetWindowHwnd();
  } else {
    mFirefoxHwnd = nullptr;
  }
}

MouseMuxDebugDialog::~MouseMuxDebugDialog() {
  StopTrackingFirefox();
  if (mDialog) {
    ::DestroyWindow(mDialog);
    mDialog = nullptr;
  }
}

std::wstring MouseMuxDebugDialog::GetExeDirectory() {
  wchar_t path[MAX_PATH];
  if (::GetModuleFileNameW(nullptr, path, MAX_PATH)) {
    ::PathRemoveFileSpecW(path);
    return std::wstring(path);
  }
  return L".";
}

void MouseMuxDebugDialog::CreateDialogWindow() {
  if (mDialog) return;

  // White background
  static HBRUSH whiteBrush = (HBRUSH)::GetStockObject(WHITE_BRUSH);

  // Load icon from exe directory (for title bar only)
  std::wstring iconPath = GetExeDirectory() + L"\\icon.ico";
  HICON hIcon = (HICON)::LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                     32, 32, LR_LOADFROMFILE);
  HICON hIconSmall = (HICON)::LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                          16, 16, LR_LOADFROMFILE);

  WNDCLASSEXW wc = {0};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DialogProc;
  wc.hInstance = ::GetModuleHandle(nullptr);
  wc.hCursor = ::LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = whiteBrush;
  wc.lpszClassName = L"MouseMuxOwnerDialog";
  wc.hIcon = hIcon;
  wc.hIconSm = hIconSmall;
  ::RegisterClassExW(&wc);

  // Calculate initial position docked to Firefox
  int x = 100, y = 100;
  int width = 420, height = 300;  // Will be resized after controls created
  if (mFirefoxHwnd && ::IsWindow(mFirefoxHwnd)) {
    RECT ffRect;
    if (::GetWindowRect(mFirefoxHwnd, &ffRect)) {
      x = ffRect.right;  // Dock to right edge
      y = ffRect.top;
    }
  }

  // Only close button in title bar (no minimize/maximize), keep icon
  DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_MINIMIZEBOX & ~WS_THICKFRAME;
  wchar_t titleBuf[64];
  swprintf(titleBuf, 64, L"MouseMux v%S", MOUSEMUX_VERSION);
  mDialog = ::CreateWindowExW(
      WS_EX_APPWINDOW, L"MouseMuxOwnerDialog", titleBuf,
      style | WS_VISIBLE,
      x, y, width, height,
      nullptr,  // No owner - allows taskbar entry
      nullptr, ::GetModuleHandle(nullptr), this);

  // Set icon for taskbar
  if (hIcon) {
    ::SendMessage(mDialog, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
  }
  if (hIconSmall) {
    ::SendMessage(mDialog, WM_SETICON, ICON_SMALL, (LPARAM)hIconSmall);
  }

  int contentY = 10;
  int margin = 10;
  int ctrlWidth = width - 2 * margin - 20;  // Account for window borders

  // Create fonts
  HFONT largeFont = ::CreateFontW(25, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT btnFont = ::CreateFontW(25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  // Top row: [Profile dropdown (60%)] [Launch (20%)] [Hide (20%)]
  int gap = 5;
  int profileWidth = (int)(ctrlWidth * 0.60);
  int launchWidth = (int)(ctrlWidth * 0.20) - gap;
  int hideWidth = ctrlWidth - profileWidth - launchWidth - 2 * gap;

  mProfileCombo = ::CreateWindowW(L"COMBOBOX", nullptr,
                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   margin, contentY, profileWidth, 200,
                                   mDialog, (HMENU)ID_PROFILE_COMBO, nullptr, nullptr);
  ::SendMessage(mProfileCombo, WM_SETFONT, (WPARAM)largeFont, TRUE);
  LoadFirefoxProfiles();

  mLaunchBtn = ::CreateWindowW(L"BUTTON", L"Launch",
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                margin + profileWidth + gap, contentY, launchWidth, 32,
                                mDialog, (HMENU)ID_LAUNCH, nullptr, nullptr);
  ::SendMessage(mLaunchBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);

  mHideBtn = ::CreateWindowW(L"BUTTON", L"Hide",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              margin + profileWidth + gap + launchWidth + gap, contentY, hideWidth, 32,
                              mDialog, (HMENU)ID_HIDE, nullptr, nullptr);
  ::SendMessage(mHideBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);
  contentY += 38;

  // Profile hint label
  mProfileHintLabel = ::CreateWindowW(L"STATIC",
                                       L"Launch another Firefox for multi-seat setup",
                                       WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       margin, contentY, ctrlWidth, 20, mDialog, nullptr, nullptr, nullptr);
  HFONT smallFont = ::CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  ::SendMessage(mProfileHintLabel, WM_SETFONT, (WPARAM)smallFont, TRUE);
  contentY += 26;

  // Connect button
  mClaimBtn = ::CreateWindowW(L"BUTTON", L"Connect",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              margin, contentY, ctrlWidth, 32, mDialog, (HMENU)ID_CLAIM, nullptr, nullptr);
  ::SendMessage(mClaimBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);
  contentY += 38;

  // Status line with colored bullet: "● Disconnected" or "● Connected | Owner: 0x1234"
  mStatusLabel = ::CreateWindowW(L"STATIC", L"\u25CF Disconnected",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 margin, contentY, ctrlWidth, 28, mDialog, (HMENU)ID_STATUS, nullptr, nullptr);
  ::SendMessage(mStatusLabel, WM_SETFONT, (WPARAM)largeFont, TRUE);
  contentY += 32;

  // Row: [Capture Mouse] - full width
  mCaptureBtn = ::CreateWindowW(L"BUTTON", L"Capture Mouse",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                 margin, contentY, ctrlWidth, 32, mDialog, (HMENU)ID_CAPTURE_BTN, nullptr, nullptr);
  ::SendMessage(mCaptureBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);
  contentY += 38;

  // Row: [Hotkey dropdown (1/3)] [Release Owner (2/3)]
  int thirdWidth = (ctrlWidth - gap) / 3;
  int twoThirdsWidth = ctrlWidth - thirdWidth - gap;

  mHotkeyCombo = ::CreateWindowW(L"COMBOBOX", nullptr,
                                  WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  margin, contentY, thirdWidth, 200,
                                  mDialog, (HMENU)ID_HOTKEY_COMBO, nullptr, nullptr);
  ::SendMessage(mHotkeyCombo, WM_SETFONT, (WPARAM)largeFont, TRUE);

  mReleaseBtn = ::CreateWindowW(L"BUTTON", L"Release Owner",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                                 margin + thirdWidth + gap, contentY, twoThirdsWidth, 32,
                                 mDialog, (HMENU)ID_RELEASE_BTN, nullptr, nullptr);
  ::SendMessage(mReleaseBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);

  // Populate hotkey dropdown
  const wchar_t* hotkeyOptions[] = {
    L"F1", L"F2", L"F3", L"F4", L"F5", L"F6",
    L"F7", L"F8", L"F9", L"F10", L"F11", L"F12",
    L"Shift+F1", L"Shift+F2", L"Shift+F3", L"Shift+F4",
    L"Shift+F5", L"Shift+F6", L"Shift+F7", L"Shift+F8",
    L"Shift+F9", L"Shift+F10", L"Shift+F11", L"Shift+F12",
    L"Escape", L"Pause"
  };
  for (const wchar_t* opt : hotkeyOptions) {
    ::SendMessageW(mHotkeyCombo, CB_ADDSTRING, 0, (LPARAM)opt);
  }
  ::SendMessageW(mHotkeyCombo, CB_SETCURSEL, 10, 0);  // Default: F11 (index 10)
  contentY += 38;

  // Log area
  int logHeight = 180;
  mLogEdit = ::CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
      margin, contentY, ctrlWidth, logHeight, mDialog, (HMENU)ID_LOG, nullptr, nullptr);
  ::SendMessage(mLogEdit, WM_SETFONT, (WPARAM)largeFont, TRUE);
  contentY += logHeight + 15;

  // Resize dialog to fit content (contentY + title bar + border)
  int finalHeight = contentY + 45;
  ::SetWindowPos(mDialog, nullptr, 0, 0, width, finalHeight, SWP_NOMOVE | SWP_NOZORDER);

  // Start a timer to periodically update status (connection is async)
  ::SetTimer(mDialog, 1, 500, nullptr);  // 500ms update interval

  UpdateStatus();

  // Log startup info
  Log("Firefox MouseMux v%s", MOUSEMUX_VERSION);
}

void MouseMuxDebugDialog::Show() {
  if (!mDialog) {
    CreateDialogWindow();
  }

  if (mDialog) {
    // Start tracking Firefox window movement
    StartTrackingFirefox();
    // Sync position before showing
    SyncPositionToFirefox();

    ::ShowWindow(mDialog, SW_SHOWNORMAL);
    mVisible = true;
    UpdateStatus();
  }
}

void MouseMuxDebugDialog::Hide() {
  // Minimize to taskbar instead of hiding completely
  if (mDialog) {
    ::ShowWindow(mDialog, SW_MINIMIZE);
  }
  mVisible = false;
}

LRESULT CALLBACK MouseMuxDebugDialog::DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  MouseMuxDebugDialog* self = nullptr;

  if (msg == WM_CREATE) {
    CREATESTRUCT* cs = (CREATESTRUCT*)lParam;
    self = (MouseMuxDebugDialog*)cs->lpCreateParams;
    ::SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)self);
  } else {
    self = (MouseMuxDebugDialog*)::GetWindowLongPtr(hwnd, GWLP_USERDATA);
  }

  if (self) {
    return self->HandleMessage(msg, wParam, lParam);
  }
  return ::DefWindowProc(hwnd, msg, wParam, lParam);
}

LRESULT MouseMuxDebugDialog::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  switch (msg) {
    case WM_MOUSEMUX_UPDATE:
      UpdateStatus();
      return 0;
    case WM_CTLCOLORSTATIC: {
      // Return white brush for all static controls (icon, title, labels)
      HDC hdc = (HDC)wParam;
      ::SetBkColor(hdc, RGB(255, 255, 255));
      static HBRUSH whiteBrush = ::CreateSolidBrush(RGB(255, 255, 255));
      return (LRESULT)whiteBrush;
    }
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case ID_CLAIM: OnClaimWindow(); return 0;
        case ID_HIDE: Hide(); return 0;
        case ID_LAUNCH: {
          int sel = (int)::SendMessage(mProfileCombo, CB_GETCURSEL, 0, 0);
          if (sel >= 0) {
            LaunchWithProfile(sel);
          }
          return 0;
        }
        case ID_CAPTURE_BTN:
          // Handle capture toggle and sync with server
          if (mClient) {
            Log("RELEASE-SOURCE: Capture button clicked");
            ToggleCapture();
            // Sync capture state with server
            uint32_t owner = mClient->GetOwnerHwid();
            if (owner != 0) {
              if (mCaptureActive) {
                mClient->RequestCapture(owner);
              } else {
                mClient->RequestReleaseCapture(owner);
              }
            }
          }
          return 0;
        case ID_RELEASE_BTN:
          ReleaseOwner();
          return 0;
        case ID_HOTKEY_COMBO:
          if (HIWORD(wParam) == CBN_SELCHANGE) {
            int sel = (int)::SendMessage(mHotkeyCombo, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < 12) {
              // F1-F12
              mCaptureHotkey = VK_F1 + sel;
              mCaptureHotkeyShift = false;
            } else if (sel >= 12 && sel < 24) {
              // Shift+F1-F12
              mCaptureHotkey = VK_F1 + (sel - 12);
              mCaptureHotkeyShift = true;
            } else if (sel == 24) {
              // Escape
              mCaptureHotkey = VK_ESCAPE;
              mCaptureHotkeyShift = false;
            } else if (sel == 25) {
              // Pause
              mCaptureHotkey = VK_PAUSE;
              mCaptureHotkeyShift = false;
            }
            wchar_t buf[32];
            ::SendMessageW(mHotkeyCombo, CB_GETLBTEXT, sel, (LPARAM)buf);
            Log("Hotkey set to: %S", buf);
          }
          return 0;
      }
      break;
    case WM_TIMER:
      if (wParam == 1) {
        // Check if we're waiting for connection to complete
        if (mPendingBlock && mClient) {
          if (mClient->IsConnected()) {
            // Connection successful - enable input blocking
            Log("Connected successfully!");
            Log("Blocking native input...");
            if (mFirefoxHwnd) {
              InputFilter::EnableForWindow(mFirefoxHwnd);
            }
            Log("Input blocked - only MouseMux input will work");
            mPendingBlock = false;
            mClaiming = false;
          } else {
            // Check for timeout (5 seconds)
            uint64_t elapsed = ::GetTickCount64() - mConnectStartTime;
            if (elapsed > 5000) {
              Log("ERROR: Connection timed out after 5 seconds");
              mPendingBlock = false;
              mClaiming = false;
            }
          }
        }
        UpdateStatus();  // Periodic status update
      }
      return 0;
    case WM_SIZE:
      if (wParam == SIZE_RESTORED) {
        // Window restored from minimized - sync position to Firefox
        SyncPositionToFirefox();
        mVisible = true;
      }
      break;
    case WM_CLOSE:
      Hide();
      return 0;
    case WM_DESTROY:
      ::KillTimer(mDialog, 1);
      mDialog = nullptr;
      return 0;
  }
  return ::DefWindowProc(mDialog, msg, wParam, lParam);
}

void MouseMuxDebugDialog::OnClaimWindow() {
  if (!mClient) {
    Log("Error: No MouseMux client set");
    return;
  }

  if (mClient->IsConnected()) {
    // Disconnect - reset capture state, unblock, then disconnect
    if (mCaptureActive) {
      SetCaptureActive(false);
    }

    Log("Unblocking input...");
    if (mFirefoxHwnd) {
      InputFilter::DisableForWindow(mFirefoxHwnd);
    }
    Log("Input unblocked");

    Log("Disconnecting from MouseMux...");
    mClient->Disconnect();
    Log("Disconnected");
    UpdateStatus();
    return;
  }

  // Connect - first try to connect, then block on success
  mClaiming = true;
  mPendingBlock = true;
  mConnectStartTime = ::GetTickCount64();
  Log("Connecting to MouseMux server...");
  UpdateStatus();

  bool started = mClient->Connect();
  if (!started) {
    Log("ERROR: Failed to start connection");
    mClaiming = false;
    mPendingBlock = false;
    UpdateStatus();
  }
  // Connection is async - timer will check for success and enable blocking
}

void MouseMuxDebugDialog::SetHoveringUser(uint32_t mouseHwid, uint32_t userId) {
  (void)mouseHwid;
  (void)userId;
}

void MouseMuxDebugDialog::ClearHoveringUser() {
}

void MouseMuxDebugDialog::UpdateStatus() {
  if (!mClaimBtn) return;

  bool connected = mClient ? mClient->IsConnected() : false;
  uint32_t ownerHwid = mClient ? mClient->GetOwnerHwid() : 0;
  bool hasOwner = ownerHwid != 0;

  // Status with colored bullet: green=connected, red=disconnected
  if (mStatusLabel) {
    wchar_t buf[128];
    if (connected) {
      if (hasOwner) {
        swprintf(buf, 128, L"\u25CF Connected | Owner: 0x%X", ownerHwid);
      } else {
        swprintf(buf, 128, L"\u25CF Connected | No owner");
      }
    } else if (mClaiming) {
      swprintf(buf, 128, L"\u25CB Connecting...");
    } else {
      swprintf(buf, 128, L"\u25CF Disconnected");
    }
    ::SetWindowTextW(mStatusLabel, buf);
  }

  // Connect/Disconnect button
  if (connected) {
    ::SetWindowTextW(mClaimBtn, L"Disconnect");
    ::EnableWindow(mClaimBtn, TRUE);
    mClaiming = false;
  } else if (mClaiming) {
    ::SetWindowTextW(mClaimBtn, L"Connecting...");
    ::EnableWindow(mClaimBtn, FALSE);
  } else {
    ::SetWindowTextW(mClaimBtn, L"Connect");
    ::EnableWindow(mClaimBtn, TRUE);
  }

  // Capture button: only enabled when connected AND has owner
  if (mCaptureBtn) {
    bool canCapture = connected && hasOwner;
    ::EnableWindow(mCaptureBtn, canCapture ? TRUE : FALSE);
    ::SetWindowTextW(mCaptureBtn, mCaptureActive ? L"Release Capture" : L"Capture Mouse");
  }

  // Release button: only enabled when has owner
  if (mReleaseBtn) {
    ::EnableWindow(mReleaseBtn, hasOwner ? TRUE : FALSE);
  }
}

void MouseMuxDebugDialog::Log(const char* aFormat, ...) {
  char buf[512];
  va_list args;
  va_start(args, aFormat);
  vsnprintf(buf, sizeof(buf), aFormat, args);
  va_end(args);
  AppendLog(buf);
}

void MouseMuxDebugDialog::AppendLog(const char* text) {
  if (!mLogEdit) return;

  std::lock_guard<std::mutex> lock(mLogMutex);

  mLogLines.push_back(text);
  while (mLogLines.size() > 100) {
    mLogLines.erase(mLogLines.begin());
  }

  std::string fullText;
  for (const auto& line : mLogLines) {
    fullText += line;
    fullText += "\r\n";
  }

  ::SetWindowTextA(mLogEdit, fullText.c_str());
  int lineCount = (int)::SendMessage(mLogEdit, EM_GETLINECOUNT, 0, 0);
  ::SendMessage(mLogEdit, EM_LINESCROLL, 0, lineCount);

  // Also update status when log changes (connection state may have changed)
  UpdateStatus();
}

void MouseMuxDebugDialog::SyncPositionToFirefox() {
  if (!mDialog || !mFirefoxHwnd || !::IsWindow(mFirefoxHwnd)) return;

  RECT ffRect;
  if (!::GetWindowRect(mFirefoxHwnd, &ffRect)) return;

  // Position dialog to the right of Firefox, keep same size
  int x = ffRect.right;
  int y = ffRect.top;

  ::SetWindowPos(mDialog, nullptr, x, y, 0, 0,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSIZE);
}

void MouseMuxDebugDialog::StartTrackingFirefox() {
  if (!mFirefoxHwnd || !::IsWindow(mFirefoxHwnd)) return;

  // Use SetWindowSubclass for safe subclassing
  ::SetWindowSubclass(mFirefoxHwnd, FirefoxSubclassProc, SUBCLASS_ID,
                      (DWORD_PTR)this);
}

void MouseMuxDebugDialog::StopTrackingFirefox() {
  if (!mFirefoxHwnd || !::IsWindow(mFirefoxHwnd)) return;

  ::RemoveWindowSubclass(mFirefoxHwnd, FirefoxSubclassProc, SUBCLASS_ID);
}

void MouseMuxDebugDialog::SetCaptureActive(bool aActive) {
  mCaptureActive = aActive;
  if (mCaptureBtn) {
    ::SetWindowTextW(mCaptureBtn, aActive ? L"Release Capture" : L"Capture Mouse");
  }
  Log("Capture %s", aActive ? "ACTIVE" : "released");
}

void MouseMuxDebugDialog::ToggleCapture() {
  if (!mClient || !mClient->IsConnected()) {
    Log("Cannot toggle capture - not connected");
    return;
  }
  SetCaptureActive(!mCaptureActive);
  // Actual capture/release is handled by MouseMuxClient checking IsCaptureActive()
}

void MouseMuxDebugDialog::ReleaseOwner() {
  if (!mClient) {
    Log("Cannot release owner - no client");
    return;
  }
  uint32_t ownerHwid = mClient->GetOwnerHwid();
  if (!ownerHwid) {
    Log("No owner to release");
    return;
  }

  // If capture is active, release it first (both UI and server)
  if (mCaptureActive) {
    Log("RELEASE-SOURCE: ReleaseOwner() releasing capture first");
    mClient->RequestReleaseCapture(ownerHwid);
    SetCaptureActive(false);
  }

  // Clear owner on client
  mClient->ClearOwner();
  Log("Owner released (was 0x%X)", ownerHwid);
  UpdateStatus();
}

LRESULT CALLBACK MouseMuxDebugDialog::FirefoxSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
    UINT_PTR idSubclass, DWORD_PTR refData) {
  MouseMuxDebugDialog* self = (MouseMuxDebugDialog*)refData;

  switch (msg) {
    case WM_WINDOWPOSCHANGED:
    case WM_MOVE:
    case WM_SIZE:
      // Firefox moved or resized - sync our position
      if (self && self->mVisible) {
        self->SyncPositionToFirefox();
      }
      break;

    case WM_ACTIVATE:
      // Firefox activated (e.g., taskbar click) - also show/activate dialog
      if (LOWORD(wParam) != WA_INACTIVE && self && self->mVisible && self->mDialog) {
        // Position dialog next to Firefox and bring to front
        self->SyncPositionToFirefox();
        ::SetWindowPos(self->mDialog, hwnd, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
      }
      break;

    case WM_DESTROY:
      // Firefox window is being destroyed - clean up
      ::RemoveWindowSubclass(hwnd, FirefoxSubclassProc, idSubclass);
      if (self) {
        self->mFirefoxHwnd = nullptr;
      }
      break;
  }

  return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

void MouseMuxDebugDialog::LoadFirefoxProfiles() {
  mProfileNames.clear();
  mProfilePaths.clear();

  // Get %APPDATA%\Mozilla\Firefox\profiles.ini
  wchar_t appData[MAX_PATH];
  if (!::SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData)) {
    std::wstring iniPath = std::wstring(appData) + L"\\Mozilla\\Firefox\\profiles.ini";

    // Read profiles.ini
    wchar_t buf[512];
    int profileNum = 0;
    while (true) {
      wchar_t section[32];
      swprintf(section, 32, L"Profile%d", profileNum);

      // Check if section exists by reading Name
      DWORD len = ::GetPrivateProfileStringW(section, L"Name", L"", buf, 512, iniPath.c_str());
      if (len == 0) break;

      std::wstring name = buf;
      mProfileNames.push_back(name);

      // Get path
      ::GetPrivateProfileStringW(section, L"Path", L"", buf, 512, iniPath.c_str());
      std::wstring path = buf;

      // Check if relative
      int isRelative = ::GetPrivateProfileIntW(section, L"IsRelative", 1, iniPath.c_str());
      if (isRelative) {
        path = std::wstring(appData) + L"\\Mozilla\\Firefox\\" + path;
      }
      mProfilePaths.push_back(path);

      profileNum++;
    }
  }

  // Populate dropdown
  if (mProfileCombo) {
    ::SendMessageW(mProfileCombo, CB_RESETCONTENT, 0, 0);
    for (const auto& name : mProfileNames) {
      ::SendMessageW(mProfileCombo, CB_ADDSTRING, 0, (LPARAM)name.c_str());
    }
    if (!mProfileNames.empty()) {
      ::SendMessageW(mProfileCombo, CB_SETCURSEL, 0, 0);
    }
  }
}

void MouseMuxDebugDialog::LaunchWithProfile(int profileIndex) {
  if (profileIndex < 0 || profileIndex >= (int)mProfilePaths.size()) {
    Log("Invalid profile index");
    return;
  }

  std::wstring profilePath = mProfilePaths[profileIndex];
  std::wstring exePath = GetExeDirectory() + L"\\firefox.exe";

  // Build command line: firefox.exe -profile "path"
  std::wstring cmdLine = L"\"" + exePath + L"\" -profile \"" + profilePath + L"\"";

  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};

  if (::CreateProcessW(nullptr, (LPWSTR)cmdLine.c_str(), nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
    Log("Launched Firefox with profile: %S", mProfileNames[profileIndex].c_str());
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(pi.hThread);
  } else {
    Log("Failed to launch Firefox (error %lu)", ::GetLastError());
  }
}

}  // namespace widget
}  // namespace mozilla
