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

class MouseMuxDebugDialog {
 public:
  static MouseMuxDebugDialog* GetInstance();
  static void Shutdown();

  void Show();
  void Hide();
  bool IsVisible() const { return mVisible; }

 private:
  MouseMuxDebugDialog();
  ~MouseMuxDebugDialog();

  void CreateDialogWindow();
  static LRESULT CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

  void OnToggleConnect();
  void OnToggleBlock();
  void UpdateStatus();
  void Log(const char* aFormat, ...);
  void AppendLog(const char* text);

  static MouseMuxDebugDialog* sInstance;

  HWND mDialog = nullptr;
  HWND mStatusLabel = nullptr;
  HWND mConnectBtn = nullptr;
  HWND mBlockBtn = nullptr;
  HWND mLogEdit = nullptr;

  bool mVisible = false;
  std::vector<std::string> mLogLines;
  std::mutex mLogMutex;

  enum { ID_STATUS = 100, ID_CONNECT, ID_BLOCK, ID_LOG };
};

}  // namespace widget
}  // namespace mozilla

#endif  // widget_windows_MouseMuxDebugDialog_h
