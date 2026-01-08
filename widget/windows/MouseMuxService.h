/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_windows_MouseMuxService_h
#define widget_windows_MouseMuxService_h

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <string>
#include <map>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

// Marker in wParam high bit to identify MouseMux-injected messages
#define MOUSEMUX_MARKER 0x80000000

// Forward declaration
class nsWindow;

namespace mozilla {
namespace widget {

/**
 * MouseMuxService - Connects to MouseMux server and injects input via
 * PostMessage directly to Firefox HWNDs.
 *
 * No SendInput, no GetKeyState - only PostMessage with data from MouseMux.
 */
class MouseMuxService {
 public:
  enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting
  };

  static MouseMuxService* GetInstance();
  static void Shutdown();

  bool Connect(const wchar_t* aUrl = L"ws://localhost:41001");
  void Disconnect();
  ConnectionState GetConnectionState() const { return mConnectionState; }
  bool IsConnected() const {
    return mConnectionState == ConnectionState::Connected;
  }

  void RegisterWindow(nsWindow* aWindow);
  void UnregisterWindow(nsWindow* aWindow);

  void SetActiveHwid(nsWindow* aWindow, uint32_t aMouseHwid,
                     uint32_t aKeyboardHwid);
  uint32_t GetActiveMouseHwid(nsWindow* aWindow) const;
  void ClearActiveHwid(nsWindow* aWindow);

  void SetUserMapping(uint32_t aMouseHwid, uint32_t aKeyboardHwid);
  uint32_t GetKeyboardHwidForMouse(uint32_t aMouseHwid) const;

  using LogCallback = std::function<void(const char*)>;
  void SetLogCallback(LogCallback aCallback);

 private:
  MouseMuxService();
  ~MouseMuxService();

  void WebSocketThread();
  void HandleMessage(const std::string& aMessage);
  void HandlePointerMotion(uint32_t aHwid, int aScreenX, int aScreenY);
  void HandlePointerButton(uint32_t aHwid, int aScreenX, int aScreenY,
                           uint32_t aEventFlags);
  void HandlePointerWheel(uint32_t aHwid, int aScreenX, int aScreenY,
                          int aDelta, bool aIsHorizontal);
  void HandleKeyboard(uint32_t aHwid, uint32_t aVkey, uint32_t aMessage,
                      uint32_t aScanCode, uint32_t aFlags);
  void HandleUserList(const std::string& aMessage);

  nsWindow* FindWindowAtPoint(int aScreenX, int aScreenY);
  WPARAM BuildMouseWParam(uint32_t aHwid);
  void Log(const char* aFormat, ...);

  static MouseMuxService* sInstance;

  std::atomic<ConnectionState> mConnectionState{ConnectionState::Disconnected};
  std::wstring mServerUrl;
  SOCKET mSocket = INVALID_SOCKET;

  std::thread mWorkerThread;
  std::atomic<bool> mShouldStop{false};

  std::vector<nsWindow*> mWindows;
  mutable std::mutex mWindowsMutex;

  struct ActiveUser {
    uint32_t mouseHwid = 0;
    uint32_t keyboardHwid = 0;
  };
  std::map<nsWindow*, ActiveUser> mActiveUsers;
  mutable std::mutex mActiveUsersMutex;

  std::map<uint32_t, uint32_t> mMouseToKeyboard;
  std::map<uint32_t, uint32_t> mKeyboardToMouse;
  mutable std::mutex mUserMappingMutex;

  // Track button state per device from MouseMux events only
  std::map<uint32_t, uint32_t> mButtonState;

  struct MousePos {
    int screenX = 0;
    int screenY = 0;
  };
  std::map<uint32_t, MousePos> mLastMousePos;

  LogCallback mLogCallback;
  std::mutex mLogMutex;
};

}  // namespace widget
}  // namespace mozilla

#endif  // widget_windows_MouseMuxService_h
