param(
    [Parameter(Mandatory)]
    [string]$DesktopName,

    [Parameter(Mandatory)]
    [int]$ProcessId,

    [Parameter(Mandatory)]
    [int]$X,

    [Parameter(Mandatory)]
    [int]$Y
)

$ErrorActionPreference = 'Stop'

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;

public static class HiddenDesktopMouse
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

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetCursorPos(int x, int y);

    [DllImport("user32.dll")]
    private static extern void mouse_event(int flags, int dx, int dy, int data, IntPtr extraInfo);

    public static void Click(string desktopName, int processId, int x, int y)
    {
        const int desktopReadObjects = 0x0001;
        const int desktopWriteObjects = 0x0080;
        const int wmMouseMove = 0x0200;
        const int wmLeftButtonDown = 0x0201;
        const int wmLeftButtonUp = 0x0202;
        const int mkLeftButton = 0x0001;

        IntPtr desktop = OpenDesktop(desktopName, 0, false, desktopReadObjects | desktopWriteObjects);
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
                        throw new InvalidOperationException("The target process has no visible window.");

                    SetForegroundWindow(target);
                    SetFocus(target);
                    SetCursorPos(x, y);
                    mouse_event(2, 0, 0, 0, IntPtr.Zero);
                    Thread.Sleep(30);
                    mouse_event(4, 0, 0, 0, IntPtr.Zero);

                    IntPtr position = new IntPtr((y << 16) | (x & 0xFFFF));
                    PostMessage(target, wmMouseMove, IntPtr.Zero, position);
                    PostMessage(target, wmLeftButtonDown, new IntPtr(mkLeftButton), position);
                    PostMessage(target, wmLeftButtonUp, IntPtr.Zero, position);
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
[HiddenDesktopMouse]::Click($DesktopName, $ProcessId, $X, $Y)
