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
#include <commctrl.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

#define SUBCLASS_ID 1001

#define MOUSEMUX_VERSION "5.33"

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

  // Load icon from exe directory
  std::wstring iconPath = GetExeDirectory() + L"\\icon.ico";
  // Load 48x48 for logo display (1.5x of 32)
  HICON hIconLarge = (HICON)::LoadImageW(nullptr, iconPath.c_str(), IMAGE_ICON,
                                          48, 48, LR_LOADFROMFILE);
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
  int width = 280, height = 600;  // Default height
  if (mFirefoxHwnd && ::IsWindow(mFirefoxHwnd)) {
    RECT ffRect;
    if (::GetWindowRect(mFirefoxHwnd, &ffRect)) {
      x = ffRect.right;  // Dock to right edge
      y = ffRect.top;
      // Match Firefox height exactly
      height = ffRect.bottom - ffRect.top;
    }
  }

  // WS_EX_APPWINDOW = has taskbar entry (so user can restore after hide)
  // No owner window - owned windows don't get taskbar entries
  // We use subclassing to track Firefox position instead
  // Use WS_OVERLAPPEDWINDOW for proper taskbar integration, remove resize/maximize
  DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
  mDialog = ::CreateWindowExW(
      WS_EX_APPWINDOW, L"MouseMuxOwnerDialog", L"MouseMux",
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

  // Create fonts (18px for controls, 22px for title)
  HFONT largeFont = ::CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT btnFont = ::CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
  HFONT titleFont = ::CreateFontW(22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

  // Logo icon at top (48x48, 1.5x bigger)
  if (hIconLarge) {
    mLogoStatic = ::CreateWindowW(L"STATIC", nullptr,
                                   WS_CHILD | WS_VISIBLE | SS_ICON,
                                   margin, contentY, 48, 48, mDialog, (HMENU)ID_LOGO, nullptr, nullptr);
    ::SendMessage(mLogoStatic, STM_SETICON, (WPARAM)hIconLarge, 0);

    // Title next to icon
    mTitleLabel = ::CreateWindowW(L"STATIC", L"MouseMux",
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   margin + 56, contentY + 10, 180, 28, mDialog, (HMENU)ID_TITLE, nullptr, nullptr);
    ::SendMessage(mTitleLabel, WM_SETFONT, (WPARAM)titleFont, TRUE);
    contentY += 58;
  }

  // Connect button
  mClaimBtn = ::CreateWindowW(L"BUTTON", L"Connect",
                              WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              margin, contentY, ctrlWidth, 35, mDialog, (HMENU)ID_CLAIM, nullptr, nullptr);
  ::SendMessage(mClaimBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);
  contentY += 42;

  // Connection status
  mStatusLabel = ::CreateWindowW(L"STATIC", L"Connection: Not connected",
                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 margin, contentY, ctrlWidth, 20, mDialog, (HMENU)ID_STATUS, nullptr, nullptr);
  ::SendMessage(mStatusLabel, WM_SETFONT, (WPARAM)largeFont, TRUE);
  contentY += 22;

  // Input blocking status
  mBlockedLabel = ::CreateWindowW(L"STATIC", L"Input: Normal (not blocked)",
                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  margin, contentY, ctrlWidth, 20, mDialog, (HMENU)ID_BLOCKED, nullptr, nullptr);
  ::SendMessage(mBlockedLabel, WM_SETFONT, (WPARAM)largeFont, TRUE);
  contentY += 22;

  // Owner status
  mOwnerLabel = ::CreateWindowW(L"STATIC", L"Owner: None",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                margin, contentY, ctrlWidth, 20, mDialog, (HMENU)ID_OWNER, nullptr, nullptr);
  ::SendMessage(mOwnerLabel, WM_SETFONT, (WPARAM)largeFont, TRUE);
  contentY += 22;

  // Hover label
  mHoverLabel = ::CreateWindowW(L"STATIC", L"Hovering: -",
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                margin, contentY, ctrlWidth, 20, mDialog, (HMENU)ID_HOVER, nullptr, nullptr);
  ::SendMessage(mHoverLabel, WM_SETFONT, (WPARAM)largeFont, TRUE);
  contentY += 28;

  // Log area - fill remaining space minus room for hide button
  // Calculate remaining height (window height minus current Y minus bottom margin minus title bar minus button)
  int buttonHeight = 35;
  int buttonMargin = 10;
  int logHeight = height - contentY - margin - 30 - buttonHeight - buttonMargin;  // 30 for title bar
  if (logHeight < 300) logHeight = 300;  // Minimum height (3x taller)

  mLogEdit = ::CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
      margin, contentY, ctrlWidth, logHeight, mDialog, (HMENU)ID_LOG, nullptr, nullptr);
  ::SendMessage(mLogEdit, WM_SETFONT, (WPARAM)largeFont, TRUE);
  contentY += logHeight + buttonMargin;

  // Minimize button at bottom
  mHideBtn = ::CreateWindowW(L"BUTTON", L"Minimize",
                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                             margin, contentY, ctrlWidth, buttonHeight, mDialog, (HMENU)ID_HIDE, nullptr, nullptr);
  ::SendMessage(mHideBtn, WM_SETFONT, (WPARAM)btnFont, TRUE);

  // Start a timer to periodically update status (connection is async)
  ::SetTimer(mDialog, 1, 500, nullptr);  // 500ms update interval

  UpdateStatus();

  // Log startup info
  Log("MouseMux Firefox Integration v%s", MOUSEMUX_VERSION);
  Log("Build date: %s %s", __DATE__, __TIME__);
  Log("Ready - click Connect to start");
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
    // Disconnect - first unblock, then disconnect
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
  std::lock_guard<std::mutex> lock(mHoverMutex);
  mHoveringMouseHwid = mouseHwid;
  mHoveringUserId = userId;

  if (mHoverLabel) {
    wchar_t buf[128];
    if (userId > 0) {
      swprintf(buf, 128, L"Hovering: User %u (mouse 0x%X)", userId, mouseHwid);
    } else {
      swprintf(buf, 128, L"Hovering: Mouse 0x%X", mouseHwid);
    }
    ::SetWindowTextW(mHoverLabel, buf);
  }
}

void MouseMuxDebugDialog::ClearHoveringUser() {
  std::lock_guard<std::mutex> lock(mHoverMutex);
  mHoveringMouseHwid = 0;
  mHoveringUserId = 0;

  if (mHoverLabel) {
    ::SetWindowTextW(mHoverLabel, L"Hovering: -");
  }
}

void MouseMuxDebugDialog::UpdateStatus() {
  if (!mClaimBtn) return;

  bool connected = mClient ? mClient->IsConnected() : false;
  uint32_t ownerHwid = mClient ? mClient->GetOwnerHwid() : 0;
  bool blocked = mFirefoxHwnd ? InputFilter::IsEnabledForWindow(mFirefoxHwnd) : false;

  // Connection status
  if (mStatusLabel) {
    wchar_t buf[128];
    if (connected) {
      swprintf(buf, 128, L"Connection: Connected");
    } else if (mClaiming) {
      swprintf(buf, 128, L"Connection: Connecting...");
    } else {
      swprintf(buf, 128, L"Connection: Not connected");
    }
    ::SetWindowTextW(mStatusLabel, buf);
  }

  // Input blocking status
  if (mBlockedLabel) {
    if (blocked) {
      ::SetWindowTextW(mBlockedLabel, L"Input: BLOCKED (MouseMux only)");
    } else {
      ::SetWindowTextW(mBlockedLabel, L"Input: Normal (native input)");
    }
  }

  // Owner status
  if (mOwnerLabel) {
    wchar_t buf[128];
    if (ownerHwid) {
      swprintf(buf, 128, L"Owner: 0x%X", ownerHwid);
    } else {
      swprintf(buf, 128, L"Owner: None (click to claim)");
    }
    ::SetWindowTextW(mOwnerLabel, buf);
  }

  // Update button text based on state
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

  // Get current dialog size
  RECT dlgRect;
  if (!::GetWindowRect(mDialog, &dlgRect)) return;
  int dlgWidth = dlgRect.right - dlgRect.left;

  // Position dialog to the right of Firefox, matching height
  int x = ffRect.right;
  int y = ffRect.top;
  int height = ffRect.bottom - ffRect.top;

  ::SetWindowPos(mDialog, nullptr, x, y, dlgWidth, height,
                 SWP_NOACTIVATE | SWP_NOZORDER);
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

}  // namespace widget
}  // namespace mozilla
