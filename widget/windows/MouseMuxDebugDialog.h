/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_windows_MouseMuxDebugDialog_h
#define widget_windows_MouseMuxDebugDialog_h

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>

namespace mozilla {
namespace widget {

class MouseMuxClient;

class MouseMuxDebugDialog {
 public:
  static MouseMuxDebugDialog* GetInstance();
  static void Shutdown();

  void Show();
  void Hide();
  bool IsVisible() const { return mVisible; }

  // Called by MouseMuxClient when mouse hovers over this dialog
  void SetHoveringUser(uint32_t mouseHwid, uint32_t userId);
  void ClearHoveringUser();

  // Set the client to use for connection/status
  void SetClient(MouseMuxClient* client);

  // Get dialog HWND (for hover detection)
  HWND GetDialogHwnd() const { return mDialog; }

 private:
  MouseMuxDebugDialog();
  ~MouseMuxDebugDialog();

  void CreateDialogWindow();
  static LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

  void OnClaimWindow();
  void UpdateStatus();
  void Log(const char* aFormat, ...);
  void AppendLog(const char* text);
  std::wstring GetExeDirectory();
  void SyncPositionToFirefox();
  void StartTrackingFirefox();
  void StopTrackingFirefox();
  static LRESULT CALLBACK FirefoxSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                               LPARAM lParam, UINT_PTR idSubclass,
                                               DWORD_PTR refData);

  static MouseMuxDebugDialog* sInstance;
  MouseMuxClient* mClient = nullptr;
  HWND mFirefoxHwnd = nullptr;       // Firefox window we're docked to

  HWND mDialog = nullptr;
  HWND mLogoStatic = nullptr;      // Logo icon
  HWND mTitleLabel = nullptr;      // "MouseMux" title
  HWND mStatusLabel = nullptr;     // Connection status
  HWND mBlockedLabel = nullptr;    // Input blocking status
  HWND mOwnerLabel = nullptr;      // Owner status
  HWND mHoverLabel = nullptr;      // Shows hovering user
  HWND mClaimBtn = nullptr;        // Connect button
  HWND mLogEdit = nullptr;         // Log output
  HWND mHideBtn = nullptr;         // Hide button
  HWND mCaptureBtn = nullptr;      // Capture toggle button
  HWND mHotkeyCombo = nullptr;     // Hotkey dropdown

  bool mVisible = false;
  bool mCaptureActive = false;     // Capture currently active
  uint8_t mCaptureHotkey = VK_F11; // Hotkey VK code
  bool mCaptureHotkeyShift = false; // Hotkey requires Shift
  bool mClaiming = false;          // True while connecting
  bool mPendingBlock = false;      // True if we need to block after connect
  uint64_t mConnectStartTime = 0;  // When connect was started (for timeout)
  uint32_t mHoveringMouseHwid = 0;
  uint32_t mHoveringUserId = 0;

  std::vector<std::string> mLogLines;
  std::mutex mLogMutex;
  std::mutex mHoverMutex;

  enum {
    ID_LOGO = 100,
    ID_TITLE,
    ID_STATUS,
    ID_BLOCKED,
    ID_OWNER,
    ID_HOVER,
    ID_CLAIM,
    ID_CAPTURE_BTN,
    ID_HOTKEY_COMBO,
    ID_LOG,
    ID_HIDE
  };

 public:
  bool IsCaptureActive() const { return mCaptureActive; }
  void SetCaptureActive(bool aActive);
  void ToggleCapture();
  uint8_t GetCaptureHotkey() const { return mCaptureHotkey; }
  bool GetCaptureHotkeyShift() const { return mCaptureHotkeyShift; }
  HWND GetCaptureButtonHwnd() const { return mCaptureBtn; }
};

}  // namespace widget
}  // namespace mozilla

#endif  // widget_windows_MouseMuxDebugDialog_h
