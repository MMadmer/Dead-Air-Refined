param(
    [Parameter(Mandatory)]
    [string]$DesktopName,

    [Parameter(Mandatory)]
    [int]$ProcessId,

    [Parameter(Mandatory)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;

public static class HiddenDesktopCaptureNative
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

    [DllImport("user32.dll")]
    private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumDesktopWindows(IntPtr desktop, EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out int processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetClientRect(IntPtr window, out RECT rectangle);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool PrintWindow(IntPtr window, IntPtr deviceContext, uint flags);

    public static long[] FindWindow(string desktopName, int processId)
    {
        const int desktopReadObjects = 0x0001;
        const int desktopWriteObjects = 0x0080;
        IntPtr desktop = OpenDesktop(desktopName, 0, false, desktopReadObjects | desktopWriteObjects);
        if (desktop == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        long[] result = null;
        Exception failure = null;
        var captureThread = new Thread(() =>
        {
            IntPtr previousDpiContext = SetThreadDpiAwarenessContext(new IntPtr(-4));
            try
            {
                if (!SetThreadDesktop(desktop))
                    throw new Win32Exception(Marshal.GetLastWin32Error());

                IntPtr target = IntPtr.Zero;
                int targetWidth = 0;
                int targetHeight = 0;
                long largestArea = 0;
                EnumDesktopWindows(desktop, (window, parameter) =>
                {
                    GetWindowThreadProcessId(window, out int ownerProcessId);
                    if (ownerProcessId != processId || !IsWindowVisible(window))
                        return true;

                    GetClientRect(window, out RECT rectangle);
                    int width = rectangle.Right - rectangle.Left;
                    int height = rectangle.Bottom - rectangle.Top;
                    long area = (long)width * height;
                    if (area > largestArea)
                    {
                        largestArea = area;
                        target = window;
                        targetWidth = width;
                        targetHeight = height;
                    }
                    return true;
                }, IntPtr.Zero);

                if (target == IntPtr.Zero)
                    throw new InvalidOperationException("The target process has no visible window.");

                result = new[] { target.ToInt64(), targetWidth, targetHeight };
            }
            catch (Exception exception)
            {
                failure = exception;
            }
            finally
            {
                if (previousDpiContext != IntPtr.Zero)
                    SetThreadDpiAwarenessContext(previousDpiContext);
            }
        });
        captureThread.IsBackground = true;
        captureThread.Start();
        captureThread.Join();
        CloseDesktop(desktop);

        if (failure != null)
            throw failure;
        return result;
    }

    public static void Capture(string desktopName, int processId, IntPtr deviceContext)
    {
        const int desktopReadObjects = 0x0001;
        const int desktopWriteObjects = 0x0080;
        IntPtr desktop = OpenDesktop(desktopName, 0, false, desktopReadObjects | desktopWriteObjects);
        if (desktop == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        Exception failure = null;
        var captureThread = new Thread(() =>
        {
            IntPtr previousDpiContext = SetThreadDpiAwarenessContext(new IntPtr(-4));
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
                if (!PrintWindow(target, deviceContext, 2))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            catch (Exception exception)
            {
                failure = exception;
            }
            finally
            {
                if (previousDpiContext != IntPtr.Zero)
                    SetThreadDpiAwarenessContext(previousDpiContext);
            }
        });
        captureThread.IsBackground = true;
        captureThread.Start();
        captureThread.Join();
        CloseDesktop(desktop);

        if (failure != null)
            throw failure;
    }
}
'@

Add-Type -AssemblyName System.Drawing.Common
Add-Type -TypeDefinition $nativeSource -Language CSharp

$window = [HiddenDesktopCaptureNative]::FindWindow($DesktopName, $ProcessId)
$bitmap = [Drawing.Bitmap]::new([int]$window[1], [int]$window[2])
$graphics = [Drawing.Graphics]::FromImage($bitmap)
try {
    $deviceContext = $graphics.GetHdc()
    try {
        [HiddenDesktopCaptureNative]::Capture($DesktopName, $ProcessId, $deviceContext)
    }
    finally {
        $graphics.ReleaseHdc($deviceContext)
    }
    $bitmap.Save($OutputPath, [Drawing.Imaging.ImageFormat]::Png)
}
finally {
    $graphics.Dispose()
    $bitmap.Dispose()
}
