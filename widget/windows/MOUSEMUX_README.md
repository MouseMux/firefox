# MouseMux Firefox Integration

Firefox build with MouseMux SDK integration for per-user window ownership.

## Version

- **MouseMux Integration**: v5.34
- **Firefox Base**: 148.0a1 (Nightly)
- **Platform**: Windows x64

## Features

### Multi-Mouse Support
- Connect to MouseMux server via WebSocket (`ws://localhost:41001`)
- Each mouse device has a unique hardware ID (hwid)
- First click on a window claims ownership for that user
- Only the owner's mouse/keyboard can interact with the claimed window

### Input Isolation
- Native input is blocked when connected (only MouseMux input works)
- Window dragging, resizing, and title bar buttons still work
- Keyboard events are paired to mouse devices via MouseMux user mapping

### Debug Dialog (Ctrl+Shift+M)
- Docked panel that follows Firefox window position
- Connect/Disconnect button with status logging
- Shows: connection status, input blocking state, owner hwid, hovering user
- Minimize button to minimize to taskbar
- Taskbar entry with MouseMux icon

## Usage

1. Start the MouseMux server
2. Run Firefox
3. Press `Ctrl+Shift+M` to open the MouseMux debug dialog
4. Click "Connect" to connect to MouseMux and block native input
5. Click with a MouseMux mouse to claim window ownership
6. Only that mouse/keyboard pair can now interact with the window

## Files

- `InputFilter.cpp/.h` - Per-window input blocking
- `MouseMuxClient.cpp/.h` - WebSocket client per window
- `MouseMuxDebugDialog.cpp/.h` - Debug UI dialog
- `MouseMuxService.cpp/.h` - Global MouseMux service
- `nsWindow.cpp` - Input filtering and MouseMux message handling

## Technical Details

### InputFilter
Blocks native mouse/keyboard messages when enabled for a window. Non-client area messages (WM_NCLBUTTONDOWN, etc.) are allowed so users can still drag windows by the title bar.

### MouseMuxClient
Per-window WebSocket connection to MouseMux server. Receives JSON events, converts to Windows messages, and posts to the Firefox window message queue with MOUSEMUX_MARKER flag.

### MouseMuxDebugDialog
Singleton dialog docked to Firefox window. Uses SetWindowSubclass to track Firefox window movement and stay aligned. Provides Connect/Disconnect UI, status display, and event logging.

### Message Flow
1. MouseMux server sends input events via WebSocket
2. MouseMuxClient receives and parses JSON messages
3. Events are converted to Windows messages (WM_MOUSEMOVE, WM_LBUTTONDOWN, etc.)
4. Messages are posted to Firefox window with MOUSEMUX_MARKER flag
5. nsWindow processes marked messages, native input is filtered out

## Building

```bash
# From mozilla-build shell
cd /path/to/firefox
./mach build widget/windows
./mach build toolkit/library
./mach run
```

## Repository

- GitHub: https://github.com/MouseMux/firefox
- Branch: `mousemux-owner-ui`
