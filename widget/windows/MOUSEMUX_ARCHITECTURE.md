# MouseMux Architecture (v5.57)

## Overview
MouseMux provides multi-mouse/keyboard support for Firefox on Windows. It connects
to a MouseMux server (ws://localhost:41001) and receives input events for multiple
input devices, injecting them into Firefox windows via custom WM_MOUSEMUX_* messages.

## Key Principle
**NEVER use Windows input APIs**: No SendInput, GetKeyState, GetCursorPos, etc.
All input comes from MouseMux server and is injected via PostMessage to target HWNDs.

## Components

### 1. InputFilter (InputFilter.h/cpp)
Per-window input blocking. When enabled for a window, blocks native mouse/keyboard
messages in nsWindow::ProcessMessage. Non-client area messages (WM_NCLBUTTONDOWN,
etc.) are allowed so users can still drag windows by the title bar.

- `InputFilter::EnableForWindow(hwnd)` / `DisableForWindow(hwnd)`
- `InputFilter::IsEnabledForWindow(hwnd)`
- Activated when MouseMuxClient connects and has an owner set

### 2. MouseMuxClient (MouseMuxClient.h/cpp) - PRIMARY
Per-window client that handles MouseMux connection and input injection.
Each nsWindow creates one MouseMuxClient instance.

Key features:
- WebSocket connection to MouseMux server (SDK v2.2.35 protocol)
- Receives pointer.motion, pointer.button, pointer.wheel, keyboard.key events
- Filters events to only process those within window bounds
- Tracks ownership (which hwid clicked on this window)
- Device-to-user mapping: pairs keyboard hwid to mouse hwid via user index
- Capture request/release via server protocol
- One-shot SetForegroundWindow after capture (mNeedsForeground flag)

Thread safety:
- Connect/Disconnect protected by mConnectMutex
- Socket access protected by mSocketMutex
- All UI updates go through PostMessage to UI thread

### 3. MouseMuxDebugDialog (MouseMuxDebugDialog.h/cpp)
Debug dialog docked to the Firefox window. Provides:
- Connect/Disconnect button with auto-login/logout
- Capture toggle button with configurable hotkey (default F9)
- Release Owner button
- Profile dropdown for device-to-user mapping
- Event logging window
- Uses SetWindowSubclass to track Firefox window movement

### 4. nsWindow Integration (nsWindow.cpp)

#### Initialization
`InitMouseMux()` creates a MouseMuxClient for each top-level window.

#### Input Blocking (InputFilter gate)
At the start of ProcessMessage, the InputFilter blocks native input:

```cpp
// Gecko method: block all native mouse/keyboard unconditionally
case WM_MOUSEMOVE:
case WM_LBUTTONDOWN: // etc.
  return true;  // Block native

case WM_MOUSEMUX_MOTION:
case WM_MOUSEMUX_BUTTON: // etc.
  break;  // Let custom messages through
```

Keyboard blocking:
```cpp
case WM_KEYDOWN:
case WM_KEYUP:
case WM_CHAR: // etc.
  if ((wParam & 0xFF) == VK_F12) break;  // F12 emergency always passes
  return true;  // Block all native keyboard

case WM_MOUSEMUX_KEY:
  break;  // Let custom message through
```

#### Custom Message Handlers

**WM_MOUSEMUX_MOTION**: Updates cursor position via Gecko WidgetMouseEvent(eMouseMove).

**WM_MOUSEMUX_BUTTON**: Dispatches Gecko WidgetMouseEvent(eMouseDown/eMouseUp).
- One-shot SetForegroundWindow after capture activation
- Always calls SetFocus on button down

**WM_MOUSEMUX_WHEEL**: Dispatches Gecko WidgetWheelEvent.

**WM_MOUSEMUX_KEY**: Keyboard input via ProcessKeyDownMessage/ProcessKeyUpMessage.
- Extracts vkey from LOWORD(wParam), msgType from HIWORD(wParam)
- Forwards to IMEHandler::GetFocusedWindow() if different from current window
- Calls SetFocus (internal, works without foreground)
- Calls TranslateMessage to generate WM_CHAR for NativeKey character resolution
- Calls ProcessKeyDownMessage/ProcessKeyUpMessage for Gecko dispatch
- Works without Win32 foreground (key multi-seat feature)

## Message Packing Format

### WM_MOUSEMUX_MOTION (WM_USER + 100)
- wParam: unused
- lParam: MAKELPARAM(clientX, clientY)

### WM_MOUSEMUX_BUTTON (WM_USER + 101)
- wParam: MAKEWPARAM(btnStateMK, eventFlags)
  - btnStateMK: MK_LBUTTON/MK_RBUTTON/MK_MBUTTON state
  - eventFlags: 0x01=leftdown, 0x02=leftup, 0x04=rightdown, 0x08=rightup, 0x10=middledown, 0x20=middleup
- lParam: MAKELPARAM(clientX, clientY)

### WM_MOUSEMUX_WHEEL (WM_USER + 102)
- wParam: MAKEWPARAM(flags, delta)
  - flags: 0x4000 = horizontal
  - delta: wheel delta (int16)
- lParam: MAKELPARAM(clientX, clientY)

### WM_MOUSEMUX_KEY (WM_USER + 103)
- wParam: MAKEWPARAM(vkey, msgType)
  - vkey: virtual key code (0-0xFF)
  - msgType: WM_KEYDOWN (0x100), WM_KEYUP (0x101), WM_SYSKEYDOWN (0x104), WM_SYSKEYUP (0x105)
- lParam: native keyboard lParam (repeat count, scan code bits 16-23, extended key bit 24, transition bits 30-31)

### WM_MOUSEMUX_UPDATE (WM_USER + 104)
- Used for UI updates from worker thread to UI thread

### WM_MOUSEMUX_LOG (WM_USER + 105)
- Used for log messages from worker thread to UI thread

## Keyboard Pipeline (solved in v5.55, modifiers fixed in v5.57)

The keyboard pipeline was the hardest part to get working. Key insights:

1. **TranslateMessage is required**: NativeKey (Gecko's keyboard handler) peeks the
   message queue for WM_CHAR after WM_KEYDOWN. Without TranslateMessage generating
   that WM_CHAR, NativeKey treats all keys as non-printable (no characters produced).
   Solution: call `TranslateMessage(&syntheticMsg)` before `ProcessKeyDownMessage`.

2. **SetFocus is sufficient, SetForegroundWindow is not needed**: `SetFocus(mWnd)` is
   an internal Win32 call that works without foreground. Combined with TranslateMessage,
   this allows keyboard input to work even when another application has foreground.
   This is the key enabler for multi-seat operation.

3. **Forwarding to focused child**: When WM_MOUSEMUX_KEY arrives at the top-level
   window, it checks `IMEHandler::GetFocusedWindow()`. If a different child nsWindow
   has Gecko focus, the message is forwarded there via PostMessage.

4. **Block native WM_CHAR in InputFilter** (v5.56): Firefox's message loop calls
   `TranslateMessage` BEFORE `DispatchMessage`, so native WM_KEYDOWN generates a
   WM_CHAR in the queue before InputFilter ever sees WM_KEYDOWN. Without blocking
   WM_CHAR, every key produces double input.

5. **Modifier key state: dual-thread sync** (v5.57): `SetSingleKeyState` was originally
   called only on the worker thread (in `HandleKeyboard`), but NativeKey reads modifier
   state on the UI thread (via `IS_VK_DOWN` -> `MmGetKeyState`). Race condition: the
   worker processes Ctrl-DOWN, C-DOWN, Ctrl-UP faster than the UI thread drains its
   queue, so when the UI thread processes C-DOWN, Ctrl is already UP in the buffer.
   Fix: added authoritative `SetSingleKeyState` in the `WM_MOUSEMUX_KEY` handler on the
   UI thread, right before `ProcessKeyDownMessage`. The worker thread also keeps its
   early sync (ensures buffer entry exists for code that reads state outside of message
   processing). Both threads update the same buffer; the UI thread sync is what matters
   for correct modifier detection during key processing.

## Build Flags

Defined in `MouseMuxClient.h`:
- `MOUSEMUX_DEBUG` (0/1): File logging via MOUSEMUX_KEY_LOG macro
- `MOUSEMUX_STALE_CODE` (0/1): Enable legacy/unused code paths
- `MOUSEMUX_INPUT_METHOD`: MOUSEMUX_INPUT_GECKO (current) or MOUSEMUX_INPUT_POSTMSG (legacy)

## File List
- InputFilter.h/cpp - Per-window blocking
- MouseMuxClient.h/cpp - Per-window WebSocket client
- MouseMuxDebugDialog.h/cpp - Debug dialog UI
- nsWindow.cpp - Integration point (InputFilter gate + custom message handlers)

## Hotkeys
- F9: Toggle capture mode (configurable)
- F11: Toggle debug dialog
- F12: Emergency exit (disable blocking, disconnect)

## Version History
- v4.3: Per-window MouseMuxClient
- v4.5: Fix keyboard handling, add MouseMux rules
- v4.6: Reduce logging (skip motion events)
- v4.7: Forward keyboard to focused window
- v5.0: Restore independent Block Input toggle
- v5.1: Fix keyboard - add MOUSEMUX_MARKER to keyboard events
- v5.2: Fix thread safety - use PostMessage for UI updates
- v5.8: Per-window isolation and multi-window ownership
- v5.9: Fix motion isolation
- v5.19: Strict owner isolation - first click claims
- v5.20: Keyboard isolation - only owner's paired keyboard can type
- v5.33: Debug dialog UI improvements
- v5.34: SDK v2.2.35 protocol (login, logout, ping/pong), add icon
- v5.35: Gecko-level mouse event injection (WidgetMouseEvent)
- v5.36: Add capture toggle button and configurable hotkey
- v5.38: Compact dialog UI with profile dropdown
- v5.39: Remove dead old dialog code
- v5.45: UI improvements
- v5.46: Fix owner/capture state management
- v5.47: Wait 1 second after logout before closing connection
- v5.48: Simplify click handling
- v5.49: Remove hover tracking
- v5.50: Redesign UI with grouped sections
- v5.51: Add MOUSEMUX_DEBUG and MOUSEMUX_STALE_CODE build flags
- v5.52: Fix crash-causing thread safety issues
- v5.53: Fix capture button bugs, change default hotkey to F9
- v5.54: Migrate keyboard to WM_MOUSEMUX_KEY custom message
- v5.55: Fix keyboard - TranslateMessage + SetFocus enables typing without foreground
- v5.56: Block native WM_CHAR in InputFilter to fix double key input
- v5.57: Fix modifier keys - dual-thread key state sync (worker early + UI authoritative)
