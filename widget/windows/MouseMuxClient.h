/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_windows_MouseMuxClient_h
#define widget_windows_MouseMuxClient_h

#include <winsock2.h>
#include <windows.h>
#include <stdint.h>
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

// Custom window messages for MouseMux events
#define WM_MOUSEMUX_MOTION    (WM_USER + 100)
#define WM_MOUSEMUX_BUTTON    (WM_USER + 101)
#define WM_MOUSEMUX_WHEEL     (WM_USER + 102)
#define WM_MOUSEMUX_KEY       (WM_USER + 103)
#define WM_MOUSEMUX_UPDATE    (WM_USER + 104)
#define WM_MOUSEMUX_LOG       (WM_USER + 105)

// Marker in wParam high bit to identify MouseMux-injected messages
#define MOUSEMUX_MARKER 0x80000000

// Build configuration
#define MOUSEMUX_DEBUG       0  // Enable file logging and verbose debug output
#define MOUSEMUX_STALE_CODE  0  // Enable legacy/unused code paths

// Input injection method: GECKO dispatches WidgetMouseEvent via custom WM_,
// POSTMSG uses legacy PostMessage(WM_LBUTTONDOWN, ...) with MOUSEMUX_MARKER.
#define MOUSEMUX_INPUT_GECKO    1
#define MOUSEMUX_INPUT_POSTMSG  2
#define MOUSEMUX_INPUT_METHOD   MOUSEMUX_INPUT_GECKO

namespace mozilla {
namespace widget {

/**
 * MouseMuxClient - Per-window MouseMux connection.
 *
 * Each Firefox window (nsWindow) owns one MouseMuxClient instance.
 * The client connects to the MouseMux server, receives all events,
 * filters them to only handle events within its window bounds,
 * and tracks ownership (which hwid clicked on this window).
 *
 * Thread safety:
 * - Connect/Disconnect are protected by mConnectMutex
 * - Socket access is protected by mSocketMutex
 * - All UI updates go through PostMessage to the UI thread
 */
class MouseMuxClient {
 public:
  explicit MouseMuxClient(HWND aOwnerHwnd);
  ~MouseMuxClient();

  // Non-copyable
  MouseMuxClient(const MouseMuxClient&) = delete;
  MouseMuxClient& operator=(const MouseMuxClient&) = delete;

  bool Connect(const wchar_t* aUrl = L"ws://localhost:41001");
  void Disconnect();
  bool IsConnected() const { return mConnected.load(); }

  // Ownership - which hwid clicked on this window
  uint32_t GetOwnerHwid() const { return mOwnerHwid.load(); }
  void ClearOwner() { mOwnerHwid.store(0); }

  // Capture control (called from dialog)
  void RequestCapture(uint32_t aHwid);
  void RequestReleaseCapture(uint32_t aHwid);

  // Get the Firefox window this client is attached to
  HWND GetWindowHwnd() const { return mOwnerHwnd; }

  // Debug dialog
  void ShowDebugDialog();
  void HideDebugDialog();
  bool IsDebugDialogVisible() const { return mDebugDialogVisible; }

  // Logging (thread-safe)
  void Log(const char* aFormat, ...);

 private:
  // Worker thread
  void WebSocketThread();
  void StopWorkerThread();

  // Message handling
  void HandleMessage(const std::string& aMessage);
  void ParseUserList(const std::string& aMessage);
  void HandlePointerMotion(uint32_t aHwid, int aScreenX, int aScreenY);
  void HandlePointerButton(uint32_t aHwid, int aScreenX, int aScreenY,
                           uint32_t aEventFlags);
  void HandlePointerWheel(uint32_t aHwid, int aScreenX, int aScreenY,
                          int aDelta, bool aIsHorizontal);
  void HandleKeyboard(uint32_t aHwid, uint32_t aVkey, uint32_t aMessage,
                      uint32_t aScanCode, uint32_t aFlags);

  // Helpers
  bool IsPointInWindow(int aScreenX, int aScreenY);
  POINT ScreenToClient(int aScreenX, int aScreenY);
#if MOUSEMUX_STALE_CODE
  WPARAM BuildMouseWParam(uint32_t aHwid);
#endif

  // SDK v2.2.35 protocol
  bool SendWebSocketMessage(const std::string& aMessage);
  void SendLogin();
  void SendLogout(const char* aReason);
  void SendPong();
  void SendCapture(uint32_t aHwid, uint32_t aFlags);
  void SendReleaseCapture(uint32_t aHwid);
  void UpdateDebugStatusSafe();

  // Owner window
  HWND mOwnerHwnd;
  std::atomic<uint32_t> mOwnerHwid{0};
  std::atomic<bool> mOwnerInWindow{false};   // Owner mouse is inside window

  // Connection state
  std::wstring mServerUrl;
  SOCKET mSocket = INVALID_SOCKET;
  std::atomic<bool> mConnected{false};
  std::atomic<bool> mShouldStop{false};

  // Thread management
  std::thread mWorkerThread;
  std::atomic<bool> mThreadRunning{false};  // True while worker thread is running
  std::mutex mConnectMutex;  // Protects Connect/Disconnect
  std::mutex mSocketMutex;   // Protects mSocket access

  // Per-device state
  std::map<uint32_t, uint32_t> mButtonState;
  std::mutex mButtonStateMutex;

  struct MousePos {
    int screenX = 0;
    int screenY = 0;
  };
  std::map<uint32_t, MousePos> mLastMousePos;
  std::mutex mMousePosMutex;

  std::map<uint32_t, uint32_t> mMouseToKeyboard;
  std::mutex mMappingMutex;

  // Debug dialog state
  bool mDebugDialogVisible = false;
};

}  // namespace widget
}  // namespace mozilla

#endif  // widget_windows_MouseMuxClient_h
