# MouseMux Firefox Integration

Firefox build with MouseMux SDK integration for per-user window ownership.

## Version

- **MouseMux Integration**: v5.55 (in progress)
- **Firefox Base**: 148.0a1 (Nightly)
- **Platform**: Windows x64
- **SDK Protocol**: v2.2.35

## Features

### Multi-Mouse Support
- Connect to MouseMux server via WebSocket (`ws://localhost:41001`)
- Each mouse device has a unique hardware ID (hwid)
- First click on a window claims ownership for that user
- Only the owner's mouse/keyboard can interact with the claimed window
- Keyboard devices paired to mouse via MouseMux user mapping

### Input Isolation
- Native mouse/keyboard input is blocked when connected (only MouseMux input works)
- Window dragging, resizing, and title bar buttons still work (WM_NC* messages pass through)
- Keyboard works without Win32 foreground (multi-seat compatible)
- F9 hotkey toggles capture mode
- F12 emergency exit always passes through

### Input Injection (Gecko Method)
- Mouse events dispatched as Gecko WidgetMouseEvent (not native WM_LBUTTONDOWN)
- Keyboard events dispatched via ProcessKeyDownMessage with TranslateMessage
- Custom window messages: WM_MOUSEMUX_MOTION, WM_MOUSEMUX_BUTTON, WM_MOUSEMUX_WHEEL, WM_MOUSEMUX_KEY
- No marker bit hack needed (custom message IDs separate MouseMux from native)

### Debug Dialog (F9 / Ctrl+Shift+M)
- Docked panel that follows Firefox window position
- Connect/Disconnect with auto-login/logout
- Capture toggle button with hotkey support
- Shows: connection status, input blocking state, owner hwid
- Profile dropdown for device-to-user mapping
- Grouped UI sections: Connection, Input Control, Owner

## Usage

1. Start the MouseMux server
2. Run Firefox: `./mach run`
3. Press F9 (or Ctrl+Shift+M) to open the MouseMux debug dialog
4. Click "Connect" to connect to MouseMux and block native input
5. Click "Capture" to start capture mode
6. Click with a MouseMux mouse to claim window ownership
7. Only that mouse/keyboard pair can now interact with the window
8. F12 to emergency disconnect and restore native input

## Files

- `InputFilter.cpp/.h` - Per-window native input blocking
- `MouseMuxClient.cpp/.h` - Per-window WebSocket client and event handling
- `MouseMuxDebugDialog.cpp/.h` - Debug UI dialog (docked to Firefox window)
- `nsWindow.cpp` - Input filtering, custom message handlers, Gecko event dispatch

## Build Flags

In `MouseMuxClient.h`:
- `MOUSEMUX_DEBUG` (0/1) - Enable file logging to `D:/scratch/firefox/mousemux_key.log`
- `MOUSEMUX_STALE_CODE` (0/1) - Enable legacy/unused code paths
- `MOUSEMUX_INPUT_METHOD` - `MOUSEMUX_INPUT_GECKO` (current) or `MOUSEMUX_INPUT_POSTMSG` (legacy)

## Building

```bash
# From mozilla-build shell
cd /path/to/firefox
./mach build
./mach run
```

## Repository

- Branch: `mousemux-owner-ui`
