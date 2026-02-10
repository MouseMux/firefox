/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MouseMuxClient.h"
#include "InputFilter.h"
#include "MouseMuxDebugDialog.h"
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdarg>
#include <sstream>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

#define MOUSEMUX_CLIENT_VERSION "5.52"
#define MOUSEMUX_SDK_VERSION "2.2.35"
#define MOUSEMUX_BUILD_DATE __DATE__

namespace mozilla {
namespace widget {

static bool sWinsockInitialized = false;
static std::mutex sWinsockMutex;

static bool EnsureWinsockInitialized() {
  std::lock_guard<std::mutex> lock(sWinsockMutex);
  if (!sWinsockInitialized) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
      return false;
    }
    sWinsockInitialized = true;
  }
  return true;
}

MouseMuxClient::MouseMuxClient(HWND aOwnerHwnd) : mOwnerHwnd(aOwnerHwnd) {
  Log("MouseMuxClient v%s created for HWND %p", MOUSEMUX_CLIENT_VERSION, aOwnerHwnd);
}

MouseMuxClient::~MouseMuxClient() {
  Log("MouseMuxClient destroying");
  mShouldStop.store(true);

  {
    std::lock_guard<std::mutex> sockLock(mSocketMutex);
    if (mSocket != INVALID_SOCKET) {
      ::shutdown(mSocket, SD_BOTH);
      ::closesocket(mSocket);
      mSocket = INVALID_SOCKET;
    }
  }

  // Wait for thread with timeout
  if (mWorkerThread.joinable()) {
    auto start = std::chrono::steady_clock::now();
    while (mThreadRunning.load()) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 500) {
        Log("Worker thread timeout, detaching");
        mWorkerThread.detach();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (mWorkerThread.joinable()) {
      mWorkerThread.join();
    }
  }
}

bool MouseMuxClient::Connect(const wchar_t* aUrl) {
  std::lock_guard<std::mutex> lock(mConnectMutex);

  if (mConnected.load()) {
    Log("Already connected");
    return true;
  }

  // Wait for any previous thread
  if (mWorkerThread.joinable()) {
    if (mThreadRunning.load()) {
      Log("Previous thread still running");
      return false;
    }
    mWorkerThread.join();
  }

  if (!EnsureWinsockInitialized()) {
    Log("WSAStartup failed");
    return false;
  }

  mServerUrl = aUrl ? aUrl : L"ws://localhost:41001";
  mShouldStop.store(false);

  mWorkerThread = std::thread(&MouseMuxClient::WebSocketThread, this);
  Log("Worker thread started");
  return true;
}

bool MouseMuxClient::SendWebSocketMessage(const std::string& aMessage) {
  std::lock_guard<std::mutex> lock(mSocketMutex);
  if (mSocket == INVALID_SOCKET) return false;

  size_t len = aMessage.length();
  std::vector<unsigned char> frame;

  frame.push_back(0x81);  // FIN + text opcode

  // Mask bit must be set for client-to-server messages
  if (len < 126) {
    frame.push_back(0x80 | (unsigned char)len);
  } else if (len < 65536) {
    frame.push_back(0x80 | 126);
    frame.push_back((len >> 8) & 0xFF);
    frame.push_back(len & 0xFF);
  } else {
    frame.push_back(0x80 | 127);
    for (int i = 7; i >= 0; i--) {
      frame.push_back((len >> (i * 8)) & 0xFF);
    }
  }

  // Mask key
  unsigned char mask[4] = {0x12, 0x34, 0x56, 0x78};
  frame.insert(frame.end(), mask, mask + 4);

  // Masked payload
  for (size_t i = 0; i < len; i++) {
    frame.push_back(aMessage[i] ^ mask[i % 4]);
  }

  int sent = send(mSocket, (char*)frame.data(), (int)frame.size(), 0);
  return sent == (int)frame.size();
}

void MouseMuxClient::SendLogin() {
  char msg[512];
  snprintf(msg, sizeof(msg),
    "{\"type\":\"client.login.request.A2M\","
    "\"appName\":\"Firefox MouseMux\","
    "\"appVersion\":\"%s\","
    "\"appBuildDate\":\"%s\","
    "\"sdkVersion\":\"%s\","
    "\"sdkBuildDate\":\"%s\"}",
    MOUSEMUX_CLIENT_VERSION, MOUSEMUX_BUILD_DATE,
    MOUSEMUX_SDK_VERSION, MOUSEMUX_BUILD_DATE);
  SendWebSocketMessage(msg);
  Log("Sent login message");
}

void MouseMuxClient::SendLogout(const char* aReason) {
  char msg[512];
  snprintf(msg, sizeof(msg),
    "{\"type\":\"client.logout.request.A2M\","
    "\"appName\":\"Firefox MouseMux\","
    "\"appVersion\":\"%s\","
    "\"sdkVersion\":\"%s\","
    "\"reason\":\"%s\"}",
    MOUSEMUX_CLIENT_VERSION, MOUSEMUX_SDK_VERSION, aReason);
  SendWebSocketMessage(msg);
  Log("Sent logout: %s", aReason);
}

void MouseMuxClient::SendPong() {
  SendWebSocketMessage("{\"type\":\"client.pong.request.A2M\"}");
}

void MouseMuxClient::SendCapture(uint32_t aHwid, uint32_t aFlags) {
  char msg[256];
  snprintf(msg, sizeof(msg),
           "{\"type\":\"pointer.capture.request.A2M\","
           "\"hwid\":%u,\"flag\":%u}",
           aHwid, aFlags);
  SendWebSocketMessage(msg);
  Log("Sent capture request: hwid=0x%X flags=0x%X", aHwid, aFlags);
}

void MouseMuxClient::SendReleaseCapture(uint32_t aHwid) {
  char msg[256];
  snprintf(msg, sizeof(msg),
           "{\"type\":\"pointer.capture.release.request.A2M\","
           "\"hwid\":%u}",
           aHwid);
  SendWebSocketMessage(msg);
  Log("Sent release capture: hwid=0x%X", aHwid);
}

void MouseMuxClient::RequestCapture(uint32_t aHwid) {
  SendCapture(aHwid, 0);
}

void MouseMuxClient::RequestReleaseCapture(uint32_t aHwid) {
  SendReleaseCapture(aHwid);
}

void MouseMuxClient::Disconnect() {
  // Release capture and owner
  uint32_t owner = mOwnerHwid.load();
  if (owner != 0) {
    SendReleaseCapture(owner);
  }
  mOwnerHwid.store(0);
  mOwnerInWindow.store(false);

  // Send logout and wait for server to process before closing
  if (mConnected.load()) {
    SendLogout("user");
    // Wait for server to process logout (up to 1 second)
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }

  mShouldStop.store(true);
  mConnected.store(false);

  {
    std::lock_guard<std::mutex> sockLock(mSocketMutex);
    if (mSocket != INVALID_SOCKET) {
      ::shutdown(mSocket, SD_BOTH);
      ::closesocket(mSocket);
      mSocket = INVALID_SOCKET;
    }
  }

  // Wait briefly for thread to exit, then detach if still running
  if (mWorkerThread.joinable()) {
    auto start = std::chrono::steady_clock::now();
    while (mThreadRunning.load()) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 200) {
        // Thread taking too long, detach to avoid blocking UI
        mWorkerThread.detach();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (mWorkerThread.joinable()) {
      mWorkerThread.join();
    }
  }

  UpdateDebugStatusSafe();
}

void MouseMuxClient::WebSocketThread() {
  mThreadRunning.store(true);
  Log("WebSocket thread started");

  SOCKET sock = INVALID_SOCKET;

  auto cleanup = [&]() {
    if (sock != INVALID_SOCKET) {
      ::closesocket(sock);
    }
    {
      std::lock_guard<std::mutex> lock(mSocketMutex);
      mSocket = INVALID_SOCKET;
    }
    mConnected.store(false);
    Log("WebSocket thread exiting");
    mThreadRunning.store(false);
    UpdateDebugStatusSafe();
  };

  std::wstring url = mServerUrl;
  std::wstring host = L"localhost";
  int port = 41001;

  size_t hostStart = url.find(L"://");
  if (hostStart != std::wstring::npos) {
    hostStart += 3;
    size_t portStart = url.find(L":", hostStart);
    if (portStart != std::wstring::npos) {
      host = url.substr(hostStart, portStart - hostStart);
      port = _wtoi(url.substr(portStart + 1).c_str());
    } else {
      host = url.substr(hostStart);
    }
  }

  char hostA[256] = {0};
  char portA[16] = {0};
  wcstombs(hostA, host.c_str(), sizeof(hostA) - 1);
  snprintf(portA, sizeof(portA), "%d", port);

  struct addrinfo hints = {0}, *result = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  if (getaddrinfo(hostA, portA, &hints, &result) != 0) {
    Log("getaddrinfo failed for %s:%s", hostA, portA);
    cleanup();
    return;
  }

  sock = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
  if (sock == INVALID_SOCKET) {
    Log("socket() failed: %d", WSAGetLastError());
    freeaddrinfo(result);
    cleanup();
    return;
  }

  // Set connect timeout
  DWORD connTimeout = 3000;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&connTimeout, sizeof(connTimeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&connTimeout, sizeof(connTimeout));

  if (mShouldStop.load()) {
    freeaddrinfo(result);
    cleanup();
    return;
  }

  if (connect(sock, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
    Log("connect() failed: %d", WSAGetLastError());
    freeaddrinfo(result);
    cleanup();
    return;
  }
  freeaddrinfo(result);

  {
    std::lock_guard<std::mutex> lock(mSocketMutex);
    mSocket = sock;
  }

  char request[512];
  snprintf(request, sizeof(request),
          "GET / HTTP/1.1\r\n"
          "Host: %s:%d\r\n"
          "Upgrade: websocket\r\n"
          "Connection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
          "Sec-WebSocket-Version: 13\r\n\r\n",
          hostA, port);

  if (send(sock, request, (int)strlen(request), 0) == SOCKET_ERROR) {
    Log("send() handshake failed: %d", WSAGetLastError());
    cleanup();
    return;
  }

  char response[1024];
  int recvLen = recv(sock, response, sizeof(response) - 1, 0);
  if (recvLen <= 0) {
    Log("Handshake failed - no response: %d", WSAGetLastError());
    cleanup();
    return;
  }
  response[recvLen] = '\0';

  if (strstr(response, "101") == nullptr) {
    Log("Handshake failed - expected 101, got: %.100s", response);
    cleanup();
    return;
  }

  mConnected.store(true);
  Log("Connected to MouseMux server at %s:%d", hostA, port);
  UpdateDebugStatusSafe();

  // Send login message (SDK v2.2.35 protocol)
  SendLogin();

  // Request user list for keyboard-to-mouse mapping
  SendWebSocketMessage("{\"type\":\"user.list.request.A2M\"}");
  Log("Requested user list from server");

  DWORD timeout = 100;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

  std::string messageBuffer;
  while (!mShouldStop.load()) {
    {
      std::lock_guard<std::mutex> lock(mSocketMutex);
      if (mSocket == INVALID_SOCKET) break;
    }

    unsigned char header[2];
    int headerLen = recv(sock, (char*)header, 2, 0);

    if (headerLen <= 0) {
      int err = WSAGetLastError();
      if (err == WSAETIMEDOUT) continue;
      if (err == WSAEINTR) continue;
      Log("recv() header failed: %d", err);
      break;
    }

    if (headerLen < 2) continue;

    bool fin = (header[0] & 0x80) != 0;
    int opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payloadLen = header[1] & 0x7F;

    if (payloadLen == 126) {
      unsigned char ext[2];
      if (recv(sock, (char*)ext, 2, 0) != 2) break;
      payloadLen = (ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
      unsigned char ext[8];
      if (recv(sock, (char*)ext, 8, 0) != 8) break;
      payloadLen = 0;
      for (int i = 0; i < 8; i++) {
        payloadLen = (payloadLen << 8) | ext[i];
      }
    }

    unsigned char mask[4] = {0};
    if (masked) {
      if (recv(sock, (char*)mask, 4, 0) != 4) break;
    }

    if (payloadLen > 65536) {
      Log("Payload too large: %llu bytes", (unsigned long long)payloadLen);
      break;
    }

    std::string payload;
    payload.resize((size_t)payloadLen);
    size_t received = 0;
    while (received < payloadLen && !mShouldStop.load()) {
      int chunk = recv(sock, &payload[received], (int)(payloadLen - received), 0);
      if (chunk <= 0) {
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT) continue;
        break;
      }
      received += chunk;
    }

    if (received < payloadLen) break;

    if (masked) {
      for (size_t i = 0; i < payloadLen; i++) {
        payload[i] ^= mask[i % 4];
      }
    }

    if (opcode == 0x08) {
      Log("Server sent close frame");
      break;
    }

    if (opcode == 0x09) {
      // WebSocket ping - respond with pong (opcode 0x0A) echoing payload
      std::vector<unsigned char> pongFrame;
      pongFrame.push_back(0x80 | 0x0A);  // FIN + pong opcode
      size_t pLen = payload.size();
      if (pLen < 126) {
        pongFrame.push_back(0x80 | (unsigned char)pLen);
      } else if (pLen < 65536) {
        pongFrame.push_back(0x80 | 126);
        pongFrame.push_back((pLen >> 8) & 0xFF);
        pongFrame.push_back(pLen & 0xFF);
      }
      unsigned char pongMask[4] = {0x12, 0x34, 0x56, 0x78};
      pongFrame.insert(pongFrame.end(), pongMask, pongMask + 4);
      for (size_t i = 0; i < pLen; i++) {
        pongFrame.push_back(payload[i] ^ pongMask[i % 4]);
      }
      std::lock_guard<std::mutex> lock(mSocketMutex);
      if (mSocket != INVALID_SOCKET) {
        send(mSocket, (char*)pongFrame.data(), (int)pongFrame.size(), 0);
      }
      continue;
    }

    if (opcode == 0x01 || opcode == 0x02) {
      messageBuffer += payload;
      if (fin) {
        HandleMessage(messageBuffer);
        messageBuffer.clear();
      }
    }
  }

  cleanup();
}

void MouseMuxClient::UpdateDebugStatusSafe() {
  // Thread-safe check before accessing singleton
  if (!MouseMuxDebugDialog::IsInstanceValid()) return;

  auto* dlg = MouseMuxDebugDialog::GetInstance();
  if (dlg && dlg->IsVisible()) {
    HWND hwnd = dlg->GetDialogHwnd();
    if (hwnd && ::IsWindow(hwnd)) {
      ::PostMessage(hwnd, WM_MOUSEMUX_UPDATE, 0, 0);
    }
  }
}

void MouseMuxClient::HandleMessage(const std::string& aMessage) {
  auto getString = [&](const char* key) -> std::string {
    std::string search = std::string("\"") + key + "\":\"";
    size_t pos = aMessage.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    size_t end = aMessage.find("\"", pos);
    if (end == std::string::npos) return "";
    return aMessage.substr(pos, end - pos);
  };

  auto getInt = [&](const char* key) -> int {
    std::string search = std::string("\"") + key + "\":";
    size_t pos = aMessage.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    return atoi(aMessage.c_str() + pos);
  };

  auto getUint = [&](const char* key) -> uint32_t {
    std::string search = std::string("\"") + key + "\":";
    size_t pos = aMessage.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    return (uint32_t)strtoul(aMessage.c_str() + pos, nullptr, 10);
  };

  std::string type = getString("type");

  if (type == "pointer.motion.notify.M2A") {
    HandlePointerMotion(getUint("hwid"), getInt("x"), getInt("y"));
  } else if (type == "pointer.button.notify.M2A") {
    HandlePointerButton(getUint("hwid"), getInt("x"), getInt("y"), getUint("button"));
  } else if (type == "pointer.wheel.notify.M2A") {
    bool horiz = aMessage.find("\"horizontal\":true") != std::string::npos;
    HandlePointerWheel(getUint("hwid"), getInt("x"), getInt("y"), getInt("delta"), horiz);
  } else if (type == "keyboard.key.notify.M2A") {
    HandleKeyboard(getUint("hwid"), getUint("vkey"), getUint("message"),
                   getUint("scan"), getUint("flags"));
  } else if (type == "user.list.notify.M2A") {
    ParseUserList(aMessage);
  } else if (type == "user.changed.notify.M2A" ||
             type == "user.create.notify.M2A" ||
             type == "user.dispose.notify.M2A") {
    Log("User change (%s), refreshing user list", type.c_str());
    SendWebSocketMessage("{\"type\":\"user.list.request.A2M\"}");

  } else if (type == "server.ping.notify.M2A") {
    SendPong();

  } else if (type == "server.timeout.warning.notify.M2A") {
    int minutes = getInt("minutes");
    Log("Server timeout warning: %d minutes remaining", minutes);
    wchar_t msg[256];
    swprintf(msg, 256, L"MouseMux server will shut down in %d minute%s.",
             minutes, minutes == 1 ? L"" : L"s");
    std::wstring msgStr(msg);
    std::thread([msgStr]() {
      ::MessageBoxW(nullptr, msgStr.c_str(), L"MouseMux Timeout Warning",
                    MB_OK | MB_ICONWARNING | MB_SYSTEMMODAL);
    }).detach();

  } else if (type == "server.timeout.stopped.notify.M2A") {
    std::string reason = getString("reason");
    Log("Server timeout stopped: %s", reason.empty() ? "(no reason)" : reason.c_str());
    wchar_t msg[256];
    if (!reason.empty()) {
      wchar_t reasonW[128];
      mbstowcs(reasonW, reason.c_str(), 127);
      reasonW[127] = L'\0';
      swprintf(msg, 256, L"MouseMux server has timed out: %s", reasonW);
    } else {
      swprintf(msg, 256, L"MouseMux server has timed out.");
    }
    std::wstring msgStr(msg);
    std::thread([msgStr]() {
      ::MessageBoxW(nullptr, msgStr.c_str(), L"MouseMux Timeout",
                    MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
    }).detach();
    mShouldStop.store(true);

  } else if (type == "server.shutdown.notify.M2A") {
    Log("Server shutting down");
    mShouldStop.store(true);
  }
}

void MouseMuxClient::ParseUserList(const std::string& aMessage) {
  std::lock_guard<std::mutex> lock(mMappingMutex);
  mMouseToKeyboard.clear();

  // Format: {"type":"user.list.notify.M2A","users":[{devices:[{hwid,type},...]},...]}
  // Each user has devices array with type "pointer" or "keyboard"
  size_t pos = aMessage.find("\"users\":");
  if (pos == std::string::npos) {
    Log("ParseUserList: no users array found");
    return;
  }

  // Parse each user object
  size_t searchPos = pos;
  while (true) {
    // Find next "devices" array
    size_t devicesPos = aMessage.find("\"devices\":", searchPos);
    if (devicesPos == std::string::npos) break;

    // Find the devices array bounds
    size_t devArrayStart = aMessage.find("[", devicesPos);
    if (devArrayStart == std::string::npos) break;

    // Find matching closing bracket (handle nested objects)
    int depth = 1;
    size_t devArrayEnd = devArrayStart + 1;
    while (depth > 0 && devArrayEnd < aMessage.length()) {
      if (aMessage[devArrayEnd] == '[') depth++;
      else if (aMessage[devArrayEnd] == ']') depth--;
      devArrayEnd++;
    }

    std::string devicesStr = aMessage.substr(devArrayStart, devArrayEnd - devArrayStart);

    // Extract pointer and keyboard hwids from this user's devices
    uint32_t pointerHwid = 0;
    uint32_t keyboardHwid = 0;

    size_t devPos = 0;
    while ((devPos = devicesStr.find("\"hwid\":", devPos)) != std::string::npos) {
      uint32_t hwid = (uint32_t)strtoul(devicesStr.c_str() + devPos + 7, nullptr, 10);

      // Find type for this device (look backwards for "type" before this hwid, or forwards)
      size_t typePos = devicesStr.rfind("\"type\":", devPos);
      size_t nextTypePos = devicesStr.find("\"type\":", devPos);

      // Use whichever is closer/more relevant to this device object
      std::string devType;
      size_t checkPos = (nextTypePos != std::string::npos && nextTypePos < devPos + 50)
                        ? nextTypePos : typePos;
      if (checkPos != std::string::npos) {
        size_t quoteStart = devicesStr.find("\"", checkPos + 7);
        size_t quoteEnd = devicesStr.find("\"", quoteStart + 1);
        if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
          devType = devicesStr.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
      }

      if (devType == "pointer" && hwid) {
        pointerHwid = hwid;
      } else if (devType == "keyboard" && hwid) {
        keyboardHwid = hwid;
      }

      devPos += 7;
    }

    if (pointerHwid && keyboardHwid) {
      mMouseToKeyboard[pointerHwid] = keyboardHwid;
      Log("User mapping: mouse 0x%X -> keyboard 0x%X", pointerHwid, keyboardHwid);
    }

    searchPos = devArrayEnd;
  }

  Log("User list updated: %zu mappings", mMouseToKeyboard.size());
}

bool MouseMuxClient::IsPointInWindow(int aScreenX, int aScreenY) {
  HWND hwnd = mOwnerHwnd;
  if (!hwnd || !::IsWindow(hwnd)) return false;

  RECT rect;
  if (!::GetWindowRect(hwnd, &rect)) return false;

  return aScreenX >= rect.left && aScreenX < rect.right &&
         aScreenY >= rect.top && aScreenY < rect.bottom;
}

POINT MouseMuxClient::ScreenToClient(int aScreenX, int aScreenY) {
  POINT pt = {aScreenX, aScreenY};
  if (mOwnerHwnd) {
    ::ScreenToClient(mOwnerHwnd, &pt);
  }
  return pt;
}

#if MOUSEMUX_STALE_CODE
WPARAM MouseMuxClient::BuildMouseWParam(uint32_t aHwid) {
  WPARAM wParam = MOUSEMUX_MARKER;

  std::lock_guard<std::mutex> lock(mButtonStateMutex);
  auto it = mButtonState.find(aHwid);
  if (it != mButtonState.end()) {
    uint32_t state = it->second;
    if (state & 0x01) wParam |= MK_LBUTTON;
    if (state & 0x04) wParam |= MK_RBUTTON;
    if (state & 0x10) wParam |= MK_MBUTTON;
  }

  return wParam;
}
#endif

void MouseMuxClient::HandlePointerMotion(uint32_t aHwid, int aScreenX, int aScreenY) {
  {
    std::lock_guard<std::mutex> lock(mMousePosMutex);
    mLastMousePos[aHwid] = {aScreenX, aScreenY};
  }

  uint32_t owner = mOwnerHwid.load();
  bool isOwner = (aHwid == owner);
  bool inWindow = IsPointInWindow(aScreenX, aScreenY);

#if MOUSEMUX_DEBUG
  static int motionCount = 0;
  if (++motionCount % 100 == 0) {
    Log("MOTION[%d] hwid=0x%X pos=(%d,%d) owner=0x%X isOwner=%d inWin=%d",
        motionCount, aHwid, aScreenX, aScreenY, owner, isOwner, inWindow);
  }
#endif

  // Track owner in/out of window (for status display only)
  if (isOwner) {
    mOwnerInWindow.store(inWindow);
  }

  // Hover tracking on debug dialog removed - was calling WindowFromPoint
  // on every motion which may have caused side effects

  // Only process from owner - no hover (prevents interference)
  if (!isOwner) return;
  




  if (!mOwnerHwnd) return;

  POINT clientPt = ScreenToClient(aScreenX, aScreenY);
  LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);

  WPARAM wp = 0;
  {
    std::lock_guard<std::mutex> lock(mButtonStateMutex);
    auto it = mButtonState.find(aHwid);
    if (it != mButtonState.end()) {
      uint32_t state = it->second;
      if (state & 0x01) wp |= MK_LBUTTON;
      if (state & 0x04) wp |= MK_RBUTTON;
      if (state & 0x10) wp |= MK_MBUTTON;
    }
  }
  ::PostMessage(mOwnerHwnd, WM_MOUSEMUX_MOTION, wp, lParam);
}

void MouseMuxClient::HandlePointerButton(uint32_t aHwid, int aScreenX, int aScreenY,
                                         uint32_t aEventFlags) {
  {
    std::lock_guard<std::mutex> lock(mMousePosMutex);
    mLastMousePos[aHwid] = {aScreenX, aScreenY};
  }

#if MOUSEMUX_DEBUG
  Log("BUTTON hwid=0x%X flags=0x%X at (%d,%d)", aHwid, aEventFlags, aScreenX, aScreenY);
#endif

  bool leftDown = (aEventFlags & 0x01) != 0;
  bool leftUp = (aEventFlags & 0x02) != 0;
  bool rightDown = (aEventFlags & 0x04) != 0;
  bool rightUp = (aEventFlags & 0x08) != 0;
  bool middleDown = (aEventFlags & 0x10) != 0;
  bool middleUp = (aEventFlags & 0x20) != 0;

  {
    std::lock_guard<std::mutex> lock(mButtonStateMutex);
    uint32_t& state = mButtonState[aHwid];
    if (leftDown) state |= 0x01;
    if (leftUp) state &= ~0x01;
    if (rightDown) state |= 0x04;
    if (rightUp) state &= ~0x04;
    if (middleDown) state |= 0x10;
    if (middleUp) state &= ~0x10;
  }

  bool isButtonDown = leftDown || rightDown || middleDown;
  uint32_t owner = mOwnerHwid.load();
  bool isOwner = (aHwid == owner);
  bool inWindow = IsPointInWindow(aScreenX, aScreenY);

  // Owner is only cleared via Release Owner button - not when clicking outside

  // Set owner on first click inside window (if no current owner)
  if (isButtonDown && inWindow && owner == 0) {
    mOwnerHwid.store(aHwid);
    Log("New owner: hwid=0x%X (locked)", aHwid);
    UpdateDebugStatusSafe();
    isOwner = true;
  }

  // Only process from owner (strict isolation)
  if (!isOwner) return;
  if (!mOwnerHwnd) return;

  // Clicks outside Firefox: don't forward, let native Windows handle (including dialog)
  if (!inWindow) return;

  // Get current button state for this device
  uint32_t currentState = 0;
  {
    std::lock_guard<std::mutex> lock(mButtonStateMutex);
    auto it = mButtonState.find(aHwid);
    if (it != mButtonState.end()) {
      currentState = it->second;
    }
  }

  POINT clientPt = ScreenToClient(aScreenX, aScreenY);
  LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);

  uint16_t btnStateMK = 0;
  if (currentState & 0x01) btnStateMK |= MK_LBUTTON;
  if (currentState & 0x04) btnStateMK |= MK_RBUTTON;
  if (currentState & 0x10) btnStateMK |= MK_MBUTTON;
  WPARAM wp = MAKEWPARAM(btnStateMK, aEventFlags);
  ::PostMessage(mOwnerHwnd, WM_MOUSEMUX_BUTTON, wp, lParam);
}

void MouseMuxClient::HandlePointerWheel(uint32_t aHwid, int aScreenX, int aScreenY,
                                        int aDelta, bool aIsHorizontal) {
  uint32_t owner = mOwnerHwid.load();
  bool isOwner = (aHwid == owner);

  // Only process from owner (strict isolation)
  if (!isOwner) return;
  if (!mOwnerHwnd) return;

  POINT clientPt = ScreenToClient(aScreenX, aScreenY);
  LPARAM lParam = MAKELPARAM(clientPt.x, clientPt.y);

  uint16_t btnState = 0;
  {
    std::lock_guard<std::mutex> lock(mButtonStateMutex);
    auto it = mButtonState.find(aHwid);
    if (it != mButtonState.end()) {
      uint32_t state = it->second;
      if (state & 0x01) btnState |= MK_LBUTTON;
      if (state & 0x04) btnState |= MK_RBUTTON;
      if (state & 0x10) btnState |= MK_MBUTTON;
    }
  }
  uint16_t flags = btnState | (aIsHorizontal ? 0x4000 : 0);
  WPARAM wp = MAKEWPARAM(flags, (uint16_t)(int16_t)aDelta);
  ::PostMessage(mOwnerHwnd, WM_MOUSEMUX_WHEEL, wp, lParam);
}

void MouseMuxClient::HandleKeyboard(uint32_t aHwid, uint32_t aVkey, uint32_t aMessage,
                                    uint32_t aScanCode, uint32_t aFlags) {
  uint32_t owner = mOwnerHwid.load();
  if (owner == 0) return;

  // Find which mouse this keyboard belongs to
  uint32_t mouseHwid = 0;
  {
    std::lock_guard<std::mutex> lock(mMappingMutex);
    for (const auto& pair : mMouseToKeyboard) {
      if (pair.second == aHwid) {
        mouseHwid = pair.first;
        break;
      }
    }
  }

  // Only accept keyboard input from the owner's paired keyboard
  if (mouseHwid != owner) return;
  if (!mOwnerHwnd) return;

  // Determine if key is pressed or released
  bool isKeyDown = (aMessage == WM_KEYDOWN || aMessage == WM_SYSKEYDOWN);
  bool isKeyUp = (aMessage == WM_KEYUP || aMessage == WM_SYSKEYUP);

  // Check for capture hotkey (only on keydown)
  if (isKeyDown && MouseMuxDebugDialog::IsInstanceValid()) {
    auto* dlg = MouseMuxDebugDialog::GetInstance();
    if (dlg) {
      uint8_t hotkey = dlg->GetCaptureHotkey();
      bool needShift = dlg->GetCaptureHotkeyShift();
      bool shiftHeld = InputFilter::IsKeyDown(mOwnerHwnd, VK_SHIFT);

      if (aVkey == hotkey && shiftHeld == needShift) {
        if (owner != 0) {
          dlg->ToggleCapture();
          if (dlg->IsCaptureActive()) {
            RequestCapture(owner);
          } else {
            RequestReleaseCapture(owner);
          }
        }
        return;  // Don't dispatch hotkey to Firefox
      }
    }
  }

  // Sync key state to InputFilter for this window
  if (isKeyDown || isKeyUp) {
    // Handle toggle keys (CapsLock, NumLock, ScrollLock)
    bool isToggleKey = (aVkey == VK_CAPITAL || aVkey == VK_NUMLOCK || aVkey == VK_SCROLL);
    if (isToggleKey && isKeyDown) {
      // Toggle keys flip their toggle state on keydown
      // For now, just set as pressed - full toggle tracking would need state
      InputFilter::SetSingleKeyState(mOwnerHwnd, aVkey, true, false);
    } else {
      InputFilter::SetSingleKeyState(mOwnerHwnd, aVkey, isKeyDown, false);
    }

    // Also update modifier keys state (Shift, Ctrl, Alt)
    if (aVkey == VK_SHIFT || aVkey == VK_LSHIFT || aVkey == VK_RSHIFT) {
      InputFilter::SetSingleKeyState(mOwnerHwnd, VK_SHIFT, isKeyDown, false);
    }
    if (aVkey == VK_CONTROL || aVkey == VK_LCONTROL || aVkey == VK_RCONTROL) {
      InputFilter::SetSingleKeyState(mOwnerHwnd, VK_CONTROL, isKeyDown, false);
    }
    if (aVkey == VK_MENU || aVkey == VK_LMENU || aVkey == VK_RMENU) {
      InputFilter::SetSingleKeyState(mOwnerHwnd, VK_MENU, isKeyDown, false);
    }
  }

  // Build lParam for the message
  LPARAM lParam = 1;  // repeat count
  lParam |= (aScanCode & 0xFF) << 16;
  if (aFlags & 0x01) lParam |= (1 << 24);  // extended key

  if (isKeyUp) {
    lParam |= (1 << 30);  // previous key state
    lParam |= (1 << 31);  // transition state
  }

  WPARAM markedVkey = aVkey | MOUSEMUX_MARKER;
  ::PostMessage(mOwnerHwnd, aMessage, markedVkey, lParam);
}

void MouseMuxClient::Log(const char* aFormat, ...) {
#if MOUSEMUX_DEBUG
  char buf[512];
  va_list args;
  va_start(args, aFormat);
  vsnprintf(buf, sizeof(buf), aFormat, args);
  va_end(args);

  FILE* f = fopen("D:/scratch/firefox/mousemux_client.log", "a");
  if (f) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[v%s %02d:%02d:%02d.%03d HWND=%p] %s\n",
            MOUSEMUX_CLIENT_VERSION,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            mOwnerHwnd, buf);
    fclose(f);
  }
#else
  (void)aFormat;
#endif
}

void MouseMuxClient::ShowDebugDialog() {
  // Use the new singleton debug dialog
  auto* dialog = MouseMuxDebugDialog::GetInstance();
  if (dialog) {
    dialog->SetClient(this);
    dialog->Show();
    mDebugDialogVisible = true;
  }
}

void MouseMuxClient::HideDebugDialog() {
  if (!MouseMuxDebugDialog::IsInstanceValid()) return;
  auto* dialog = MouseMuxDebugDialog::GetInstance();
  if (dialog) {
    dialog->Hide();
  }
  mDebugDialogVisible = false;
}

}  // namespace widget
}  // namespace mozilla
