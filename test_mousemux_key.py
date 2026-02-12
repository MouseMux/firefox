#!/usr/bin/env python3
"""
Test WM_MOUSEMUX_KEY: progressive tests to isolate keyboard issues.

Test A: Physical click + SendInput keys (baseline)
Test B: Physical click + WM_MOUSEMUX_KEY
Test C: WM_MOUSEMUX_BUTTON click + WM_MOUSEMUX_KEY (full pipeline)
"""
import ctypes
import ctypes.wintypes as wt
import time
import subprocess
import sys
import os
import struct

user32 = ctypes.windll.user32

WM_USER = 0x0400
WM_MOUSEMUX_KEY = WM_USER + 103
WM_MOUSEMUX_BUTTON = WM_USER + 101
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CHAR = 0x0102

VK_SCAN = {
    0x41: 0x1E, 0x42: 0x30, 0x43: 0x2E, 0x44: 0x20,
    0x45: 0x12, 0x46: 0x21, 0x47: 0x22, 0x48: 0x23,
    0x49: 0x17, 0x4A: 0x24, 0x4B: 0x25, 0x4C: 0x26,
    0x4D: 0x32, 0x4E: 0x31, 0x4F: 0x18, 0x50: 0x19,
    0x51: 0x10, 0x52: 0x13, 0x53: 0x1F, 0x54: 0x14,
    0x55: 0x16, 0x56: 0x2F, 0x57: 0x11, 0x58: 0x2D,
    0x59: 0x15, 0x5A: 0x2C,
}

MK_LBUTTON = 0x0001

# SendInput structures
INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_SCANCODE = 0x0008


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", wt.WORD),
        ("wScan", wt.WORD),
        ("dwFlags", wt.DWORD),
        ("time", wt.DWORD),
        ("dwExtraInfo", ctypes.POINTER(ctypes.c_ulong)),
    ]


class INPUT(ctypes.Structure):
    class _INPUT_UNION(ctypes.Union):
        _fields_ = [
            ("ki", KEYBDINPUT),
            ("padding", ctypes.c_byte * 32),
        ]
    _fields_ = [
        ("type", wt.DWORD),
        ("union", _INPUT_UNION),
    ]


def MAKEWPARAM(lo, hi):
    return wt.WPARAM((lo & 0xFFFF) | ((hi & 0xFFFF) << 16))


def MAKELPARAM(lo, hi):
    return wt.LPARAM(ctypes.c_long(((hi & 0xFFFF) << 16) | (lo & 0xFFFF)).value)


def send_input_key(vkey):
    """Send key press+release via SendInput (normal Windows input pipeline)."""
    scan = VK_SCAN.get(vkey, 0)
    inputs = (INPUT * 2)()

    # Key down
    inputs[0].type = INPUT_KEYBOARD
    inputs[0].union.ki.wVk = vkey
    inputs[0].union.ki.wScan = scan
    inputs[0].union.ki.dwFlags = 0
    inputs[0].union.ki.time = 0
    inputs[0].union.ki.dwExtraInfo = None

    # Key up
    inputs[1].type = INPUT_KEYBOARD
    inputs[1].union.ki.wVk = vkey
    inputs[1].union.ki.wScan = scan
    inputs[1].union.ki.dwFlags = KEYEVENTF_KEYUP
    inputs[1].union.ki.time = 0
    inputs[1].union.ki.dwExtraInfo = None

    ret = user32.SendInput(2, ctypes.byref(inputs), ctypes.sizeof(INPUT))
    time.sleep(0.05)
    return ret


def send_input_string(text):
    """Send string via SendInput."""
    for ch in text.upper():
        vkey = ord(ch)
        if 0x41 <= vkey <= 0x5A:
            send_input_key(vkey)


def send_mousemux_key(hwnd, vkey):
    """Send a keydown + keyup via WM_MOUSEMUX_KEY."""
    scan = VK_SCAN.get(vkey, 0)

    wp = MAKEWPARAM(vkey, WM_KEYDOWN)
    lp = wt.LPARAM(1 | (scan << 16))
    user32.PostMessageW(hwnd, WM_MOUSEMUX_KEY, wp, lp)
    time.sleep(0.05)

    wp = MAKEWPARAM(vkey, WM_KEYUP)
    lp_val = 1 | (scan << 16) | (1 << 30) | (1 << 31)
    if lp_val >= 0x80000000:
        lp_val -= 0x100000000
    lp = wt.LPARAM(lp_val)
    user32.PostMessageW(hwnd, WM_MOUSEMUX_KEY, wp, lp)
    time.sleep(0.05)


def send_postmsg_key(hwnd, vkey):
    """Send native WM_KEYDOWN/WM_KEYUP via PostMessage (no marker)."""
    scan = VK_SCAN.get(vkey, 0)

    lp = wt.LPARAM(1 | (scan << 16))
    user32.PostMessageW(hwnd, WM_KEYDOWN, wt.WPARAM(vkey), lp)
    time.sleep(0.05)

    lp_val = 1 | (scan << 16) | (1 << 30) | (1 << 31)
    if lp_val >= 0x80000000:
        lp_val -= 0x100000000
    lp = wt.LPARAM(lp_val)
    user32.PostMessageW(hwnd, WM_KEYUP, wt.WPARAM(vkey), lp)
    time.sleep(0.05)


def send_wm_char(hwnd, char):
    """Send WM_CHAR directly via PostMessage."""
    user32.PostMessageW(hwnd, WM_CHAR, wt.WPARAM(ord(char)), wt.LPARAM(0))
    time.sleep(0.05)


def send_mousemux_click(hwnd, x, y):
    """Send a left button down+up via WM_MOUSEMUX_BUTTON."""
    lp = MAKELPARAM(x, y)
    wp_down = MAKEWPARAM(MK_LBUTTON, 0x01)
    user32.PostMessageW(hwnd, WM_MOUSEMUX_BUTTON, wp_down, lp)
    time.sleep(0.05)
    wp_up = MAKEWPARAM(0, 0x02)
    user32.PostMessageW(hwnd, WM_MOUSEMUX_BUTTON, wp_up, lp)
    time.sleep(0.05)


def send_mousemux_string(hwnd, text):
    """Send each character as a WM_MOUSEMUX_KEY press+release."""
    for ch in text.upper():
        vkey = ord(ch)
        if 0x41 <= vkey <= 0x5A:
            send_mousemux_key(hwnd, vkey)


def find_firefox_hwnds():
    """Find all top-level Firefox windows."""
    hwnds = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def callback(hwnd, lparam):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == "MozillaWindowClass":
            if user32.IsWindowVisible(hwnd):
                hwnds.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(callback), 0)
    return hwnds


def get_title(hwnd):
    buf = ctypes.create_unicode_buffer(1024)
    user32.GetWindowTextW(hwnd, buf, 1024)
    return buf.value


def get_client_rect(hwnd):
    rect = wt.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    return rect


def physical_click(target_hwnd, client_x, client_y):
    """Click at client coords using SetCursorPos + mouse_event."""
    pt = wt.POINT(client_x, client_y)
    user32.ClientToScreen(target_hwnd, ctypes.byref(pt))
    print(f"  Physical click: screen=({pt.x}, {pt.y})")
    user32.SetCursorPos(pt.x, pt.y)
    time.sleep(0.15)
    MOUSEEVENTF_LEFTDOWN = 0x0002
    MOUSEEVENTF_LEFTUP = 0x0004
    user32.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.05)
    user32.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.3)


def select_all_delete():
    """Ctrl+A, Delete via SendInput to clear input."""
    inputs = (INPUT * 4)()
    # Ctrl down
    inputs[0].type = INPUT_KEYBOARD
    inputs[0].union.ki.wVk = 0x11  # VK_CONTROL
    inputs[0].union.ki.dwFlags = 0
    # A down
    inputs[1].type = INPUT_KEYBOARD
    inputs[1].union.ki.wVk = 0x41  # 'A'
    inputs[1].union.ki.dwFlags = 0
    # A up
    inputs[2].type = INPUT_KEYBOARD
    inputs[2].union.ki.wVk = 0x41
    inputs[2].union.ki.dwFlags = KEYEVENTF_KEYUP
    # Ctrl up
    inputs[3].type = INPUT_KEYBOARD
    inputs[3].union.ki.wVk = 0x11
    inputs[3].union.ki.dwFlags = KEYEVENTF_KEYUP
    user32.SendInput(4, ctypes.byref(inputs), ctypes.sizeof(INPUT))
    time.sleep(0.1)
    # Delete
    send_input_key(0x2E)  # VK_DELETE
    time.sleep(0.2)


def ensure_foreground(hwnd):
    """Best-effort to make hwnd the foreground window."""
    KEYEVENTF = 0x0002
    user32.keybd_event(0xFF, 0, 0, 0)
    user32.keybd_event(0xFF, 0, KEYEVENTF, 0)
    time.sleep(0.05)
    ret = user32.SetForegroundWindow(hwnd)
    time.sleep(0.3)
    fg = user32.GetForegroundWindow()
    is_fg = (fg == hwnd)
    print(f"  SetForegroundWindow={ret}, is_foreground={is_fg}")
    return is_fg


def check_title(hwnd, expected_text):
    """Check if the window title contains MMUXTEST:{expected_text}."""
    title = get_title(hwnd)
    target = f"MMUXTEST:{expected_text}"
    ok = target in title
    print(f"  Title: {title!r}")
    print(f"  Looking for: {target!r} -> {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    test_html = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "test_mousemux_input.html")
    test_url = "file:///" + test_html.replace("\\", "/")

    firefox_exe = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "obj-x86_64-pc-windows-msvc", "dist", "bin", "firefox.exe")

    if not os.path.exists(firefox_exe):
        print(f"ERROR: Firefox not found at {firefox_exe}")
        return False

    print(f"Launching Firefox with {test_url}")
    proc = subprocess.Popen([firefox_exe, "-no-remote", test_url])
    print(f"Firefox PID: {proc.pid}")

    print("Waiting for Firefox window...")
    hwnd = None
    for i in range(30):
        time.sleep(1)
        hwnds = find_firefox_hwnds()
        for h in hwnds:
            title = get_title(h)
            if "MMUXTEST" in title:
                hwnd = h
                break
        if hwnd:
            break
        if i % 5 == 4:
            print(f"  Still waiting... ({len(hwnds)} Firefox windows found)")

    if not hwnd:
        print("ERROR: Test page didn't load in Firefox")
        proc.terminate()
        return False

    print(f"Found test window: HWND={hwnd:#x} title={get_title(hwnd)!r}")
    time.sleep(2)  # Let page fully stabilize

    rect = get_client_rect(hwnd)
    cx = (rect.right - rect.left) // 2
    cy = (rect.bottom - rect.top) // 2
    print(f"Client rect: {rect.right}x{rect.bottom}, center=({cx},{cy})")

    # Ensure Firefox is foreground
    if not ensure_foreground(hwnd):
        print("WARNING: Could not set Firefox as foreground")

    # === TEST A: Physical click + SendInput (baseline) ===
    print("\n=== TEST A: Physical click + SendInput keys (baseline) ===")
    physical_click(hwnd, cx, cy)
    time.sleep(0.5)

    print("  Sending 'de' via SendInput...")
    send_input_string("de")
    time.sleep(0.5)

    test_a = check_title(hwnd, "de")

    # === TEST B: Same window + WM_MOUSEMUX_KEY ===
    print("\n=== TEST B: WM_MOUSEMUX_KEY (input should already be focused) ===")
    # Don't clear - just append
    print("  Sending 'fg' via WM_MOUSEMUX_KEY...")
    send_mousemux_string(hwnd, "fg")
    time.sleep(0.5)

    test_b = check_title(hwnd, "defg")

    # === TEST C: PostMessage native WM_KEYDOWN (no marker) ===
    print("\n=== TEST C: PostMessage native WM_KEYDOWN ===")
    print("  Sending 'hi' via PostMessage WM_KEYDOWN...")
    for ch in "HI":
        send_postmsg_key(hwnd, ord(ch))
    time.sleep(0.5)

    test_c = check_title(hwnd, "defghi")

    # === TEST D: PostMessage WM_CHAR directly ===
    print("\n=== TEST D: PostMessage WM_CHAR ===")
    print("  Sending 'jk' via WM_CHAR...")
    for ch in "jk":
        send_wm_char(hwnd, ch)
    time.sleep(0.5)

    test_d = check_title(hwnd, "defghi" + "jk")

    # === TEST E: Clear via SendInput, then WM_MOUSEMUX_KEY (no click) ===
    print("\n=== TEST E: Clear + WM_MOUSEMUX_KEY (no click, keep existing focus) ===")
    select_all_delete()
    time.sleep(0.3)
    print("  Sending 'lm' via WM_MOUSEMUX_KEY...")
    send_mousemux_string(hwnd, "lm")
    time.sleep(0.5)

    test_e = check_title(hwnd, "lm")

    # === TEST F: Lose foreground, then WM_MOUSEMUX_KEY ===
    print("\n=== TEST F: Lose foreground + WM_MOUSEMUX_KEY ===")
    select_all_delete()
    time.sleep(0.3)
    # Steal foreground by focusing our console window
    our_hwnd = user32.GetForegroundWindow()
    print(f"  Current foreground: {our_hwnd:#x}")
    # Minimize Firefox to lose foreground
    user32.ShowWindow(hwnd, 6)  # SW_MINIMIZE
    time.sleep(0.5)
    # Restore it but don't give it foreground
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    time.sleep(0.5)
    fg = user32.GetForegroundWindow()
    print(f"  After restore, foreground: {fg:#x}, is_firefox={fg==hwnd}")
    # Physical click to re-focus the input (need foreground for this)
    ensure_foreground(hwnd)
    physical_click(hwnd, cx, cy)
    time.sleep(0.3)
    # Now explicitly lose foreground
    user32.keybd_event(0xFF, 0, 0, 0)
    user32.keybd_event(0xFF, 0, 0x0002, 0)
    time.sleep(0.05)
    # Find another window to steal focus
    notepad = subprocess.Popen(["notepad.exe"])
    time.sleep(1)
    notepad_hwnds = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def find_notepad(h, lp):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(h, buf, 256)
        if "Notepad" in buf.value and user32.IsWindowVisible(h):
            notepad_hwnds.append(h)
        return True
    user32.EnumWindows(WNDENUMPROC(find_notepad), 0)
    if notepad_hwnds:
        user32.SetForegroundWindow(notepad_hwnds[0])
        time.sleep(0.5)
    fg = user32.GetForegroundWindow()
    print(f"  Foreground after notepad: {fg:#x}, is_firefox={fg==hwnd}")

    print("  Sending 'pq' via WM_MOUSEMUX_KEY (no foreground)...")
    send_mousemux_string(hwnd, "pq")
    time.sleep(0.5)

    test_f = check_title(hwnd, "pq")

    # Clean up notepad
    notepad.terminate()

    print(f"\n=== RESULTS ===")
    tests = {
        "A (physical click + SendInput)": test_a,
        "B (WM_MOUSEMUX_KEY)": test_b,
        "C (PostMessage WM_KEYDOWN)": test_c,
        "D (PostMessage WM_CHAR)": test_d,
        "E (clear + WM_MOUSEMUX_KEY, no click)": test_e,
        "F (no foreground + WM_MOUSEMUX_KEY)": test_f,
    }
    for name, result in tests.items():
        print(f"  {name}: {'PASS' if result else 'FAIL'}")

    all_pass = all(tests.values())

    print("\nClosing Firefox...")
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    return all_pass


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
