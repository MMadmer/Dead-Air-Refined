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
using System.Threading;

public static class HiddenDesktopKeyboard
{
    private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenDesktop(string desktop, int flags, bool inherit, int desiredAccess);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool CloseDesktop(IntPtr desktop);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetThreadDesktop(IntPtr desktop);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumDesktopWindows(IntPtr desktop, EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out int processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetClientRect(IntPtr window, out RECT rectangle);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr window, int message, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr window);

    [DllImport("user32.dll")]
    private static extern void keybd_event(byte virtualKey, byte scanCode, int flags, IntPtr extraInfo);

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

        Exception failure = null;
        try
        {
            var inputThread = new Thread(() =>
            {
                try
                {
                    if (!SetThreadDesktop(desktop))
                        throw new Win32Exception(Marshal.GetLastWin32Error());

                    IntPtr target = IntPtr.Zero;
                    long largestArea = 0;
                    EnumDesktopWindows(desktop, (window, parameter) =>
                    {
                        GetWindowThreadProcessId(window, out int ownerProcessId);
                        if (ownerProcessId != processId || !IsWindowVisible(window))
                            return true;

                        GetClientRect(window, out RECT rectangle);
                        long area = (long)(rectangle.Right - rectangle.Left) * (rectangle.Bottom - rectangle.Top);
                        if (area > largestArea)
                        {
                            largestArea = area;
                            target = window;
                        }
                        return true;
                    }, IntPtr.Zero);

                    if (target == IntPtr.Zero)
                        throw new InvalidOperationException("The target process has no window on the requested desktop.");

                    SetForegroundWindow(target);
                    SetFocus(target);
                    int scanCode = (int)MapVirtualKey((uint)virtualKey, 0);
                    keybd_event((byte)virtualKey, (byte)scanCode, 0, IntPtr.Zero);
                    Thread.Sleep(30);
                    keybd_event((byte)virtualKey, (byte)scanCode, 2, IntPtr.Zero);

                    IntPtr downData = new IntPtr(1 | (scanCode << 16));
                    IntPtr upData = new IntPtr(1 | (scanCode << 16) | unchecked((int)0xC0000000));
                    PostMessage(target, wmKeyDown, new IntPtr(virtualKey), downData);
                    PostMessage(target, wmKeyUp, new IntPtr(virtualKey), upData);
                }
                catch (Exception exception)
                {
                    failure = exception;
                }
            });
            inputThread.IsBackground = true;
            inputThread.Start();
            inputThread.Join();
        }
        finally
        {
            CloseDesktop(desktop);
        }

        if (failure != null)
            throw failure;
    }
}
'@

Add-Type -TypeDefinition $nativeSource -Language CSharp
[HiddenDesktopKeyboard]::Send($DesktopName, $ProcessId, $VirtualKey)
