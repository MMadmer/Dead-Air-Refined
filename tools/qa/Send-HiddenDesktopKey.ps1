param(
    [Parameter(Mandatory)]
    [string]$DesktopName,

    [Parameter(Mandatory)]
    [int]$ProcessId,

    [Parameter(Mandatory)]
    [ValidateRange(1, 255)]
    [int]$VirtualKey
)

$ErrorActionPreference = 'Stop'

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

public static class HiddenDesktopKeyboard
{
    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenDesktop(string desktop, int flags, bool inherit, int desiredAccess);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool CloseDesktop(IntPtr desktop);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumDesktopWindows(IntPtr desktop, EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out int processId);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr window, int message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint MapVirtualKey(uint code, uint mapType);

    public static void Send(string desktopName, int processId, int virtualKey)
    {
        const int desktopReadObjects = 0x0001;
        const int desktopWriteObjects = 0x0080;
        const int wmKeyDown = 0x0100;
        const int wmKeyUp = 0x0101;

        IntPtr desktop = OpenDesktop(
            desktopName,
            0,
            false,
            desktopReadObjects | desktopWriteObjects);
        if (desktop == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        try
        {
            IntPtr target = IntPtr.Zero;
            EnumDesktopWindows(desktop, (window, parameter) =>
            {
                GetWindowThreadProcessId(window, out int ownerProcessId);
                if (ownerProcessId != processId)
                    return true;

                target = window;
                return false;
            }, IntPtr.Zero);

            if (target == IntPtr.Zero)
                throw new InvalidOperationException("The target process has no window on the requested desktop.");

            int scanCode = (int)MapVirtualKey((uint)virtualKey, 0);
            IntPtr downData = new IntPtr(1 | (scanCode << 16));
            IntPtr upData = new IntPtr(1 | (scanCode << 16) | unchecked((int)0xC0000000));
            if (!PostMessage(target, wmKeyDown, new IntPtr(virtualKey), downData) ||
                !PostMessage(target, wmKeyUp, new IntPtr(virtualKey), upData))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }
        finally
        {
            CloseDesktop(desktop);
        }
    }
}
'@

Add-Type -TypeDefinition $nativeSource -Language CSharp
[HiddenDesktopKeyboard]::Send($DesktopName, $ProcessId, $VirtualKey)
