param(
    [Parameter(Mandatory)]
    [string]$DesktopName,

    [Parameter(Mandatory)]
    [int]$ProcessId
)

$ErrorActionPreference = 'Stop'

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;

public static class HiddenDesktopFocus
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

    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr window, out RECT rectangle);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PostMessage(IntPtr window, int message, IntPtr wParam, IntPtr lParam);

    public static void Focus(string desktopName, int processId)
    {
        const int desktopReadObjects = 0x0001;
        const int desktopWriteObjects = 0x0080;
        IntPtr desktop = OpenDesktop(desktopName, 0, false, desktopReadObjects | desktopWriteObjects);
        if (desktop == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        try
        {
            Exception failure = null;
            var focusThread = new Thread(() =>
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

                        GetWindowRect(window, out RECT rectangle);
                        long area = (long)(rectangle.Right - rectangle.Left) * (rectangle.Bottom - rectangle.Top);
                        if (area > largestArea)
                        {
                            largestArea = area;
                            target = window;
                        }
                        return true;
                    }, IntPtr.Zero);

                    if (target == IntPtr.Zero)
                        throw new InvalidOperationException("The target process has no visible window.");

                    SetForegroundWindow(target);
                    SetFocus(target);
                    PostMessage(target, 0x001C, new IntPtr(1), IntPtr.Zero);
                    PostMessage(target, 0x0006, new IntPtr(1), IntPtr.Zero);
                    PostMessage(target, 0x0007, IntPtr.Zero, IntPtr.Zero);
                }
                catch (Exception exception)
                {
                    failure = exception;
                }
            });
            focusThread.IsBackground = true;
            focusThread.Start();
            focusThread.Join();

            if (failure != null)
                throw failure;
        }
        finally
        {
            CloseDesktop(desktop);
        }
    }
}
'@

Add-Type -TypeDefinition $nativeSource -Language CSharp
[HiddenDesktopFocus]::Focus($DesktopName, $ProcessId)
