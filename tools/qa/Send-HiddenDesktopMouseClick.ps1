param(
    [Parameter(Mandatory)]
    [string]$DesktopName,

    [Parameter(Mandatory)]
    [int]$ProcessId,

    [Parameter(Mandatory)]
    [int]$X,

    [Parameter(Mandatory)]
    [int]$Y,

    # Client-area coordinates of the same point. X/Y drive the physical cursor and are therefore
    # screen coordinates; the posted messages need the point in the window's client space. They
    # differ whenever the window has a border or a title bar, so a caller that knows the offset
    # passes both. Omitted, the posted messages keep using X/Y as before.
    [int]$ClientX = [int]::MinValue,

    [int]$ClientY = [int]::MinValue,

    # Move the pointer without pressing. The engine dispatches a button press straight away but
    # applies the accumulated motion only at the end of the frame's input batch, so a move and a
    # click delivered together are handled at the *previous* cursor position. Callers move first,
    # let a few frames pass, and click after that.
    [switch]$MoveOnly
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

    public static void Click(string desktopName, int processId, int x, int y, int clientX, int clientY, bool moveOnly)
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
                    IntPtr position = new IntPtr((clientY << 16) | (clientX & 0xFFFF));
                    PostMessage(target, wmMouseMove, IntPtr.Zero, position);
                    if (moveOnly)
                        return;

                    // Synthetic hardware input only reaches the window station's *input* desktop,
                    // which a hidden desktop is not, so the posted messages are what actually
                    // drives the target. The cursor call keeps the two in step when they do match.
                    SetCursorPos(x, y);
                    mouse_event(2, 0, 0, 0, IntPtr.Zero);
                    Thread.Sleep(30);
                    mouse_event(4, 0, 0, 0, IntPtr.Zero);

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

# Callers invoke this dot-sourced style many times per run, and Add-Type refuses to redefine a type
# that is already in the session.
if (-not ('HiddenDesktopMouse' -as [type])) {
    Add-Type -TypeDefinition $nativeSource -Language CSharp
}
if ($ClientX -eq [int]::MinValue) { $ClientX = $X }
if ($ClientY -eq [int]::MinValue) { $ClientY = $Y }
[HiddenDesktopMouse]::Click($DesktopName, $ProcessId, $X, $Y, $ClientX, $ClientY, [bool]$MoveOnly)
