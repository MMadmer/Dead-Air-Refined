param(
    [Parameter(Mandatory)]
    [string]$DesktopName,

    [Parameter(Mandatory)]
    [int]$ProcessId
)

$ErrorActionPreference = 'Stop'

$nativeSource = @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

public sealed class HiddenWindowInfo
{
    public long Handle { get; set; }
    public string ClassName { get; set; }
    public string Title { get; set; }
    public bool Visible { get; set; }
    public int X { get; set; }
    public int Y { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
    public long Style { get; set; }
    public long ExtendedStyle { get; set; }
}

public static class HiddenDesktopWindows
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

    [DllImport("user32.dll")]
    private static extern IntPtr SetThreadDpiAwarenessContext(IntPtr dpiContext);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool CloseDesktop(IntPtr desktop);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumDesktopWindows(IntPtr desktop, EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint GetWindowThreadProcessId(IntPtr window, out int processId);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool GetWindowRect(IntPtr window, out RECT rectangle);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr window, StringBuilder className, int maximumCount);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr window, StringBuilder title, int maximumCount);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern IntPtr GetWindowLongPtr64(IntPtr window, int index);

    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")]
    private static extern IntPtr GetWindowLong32(IntPtr window, int index);

    private static IntPtr GetWindowLongPtr(IntPtr window, int index)
    {
        return IntPtr.Size == 8 ? GetWindowLongPtr64(window, index) : GetWindowLong32(window, index);
    }

    public static HiddenWindowInfo[] Get(string desktopName, int processId)
    {
        const int desktopReadObjects = 0x0001;
        const int styleIndex = -16;
        const int extendedStyleIndex = -20;
        var previousDpiContext = SetThreadDpiAwarenessContext(new IntPtr(-4));

        IntPtr desktop = OpenDesktop(desktopName, 0, false, desktopReadObjects);
        if (desktop == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        try
        {
            var result = new List<HiddenWindowInfo>();
            EnumDesktopWindows(desktop, (window, parameter) =>
            {
                GetWindowThreadProcessId(window, out int ownerProcessId);
                if (ownerProcessId != processId)
                    return true;

                GetWindowRect(window, out RECT rectangle);
                var className = new StringBuilder(256);
                var title = new StringBuilder(512);
                GetClassName(window, className, className.Capacity);
                GetWindowText(window, title, title.Capacity);
                result.Add(new HiddenWindowInfo
                {
                    Handle = window.ToInt64(),
                    ClassName = className.ToString(),
                    Title = title.ToString(),
                    Visible = IsWindowVisible(window),
                    X = rectangle.Left,
                    Y = rectangle.Top,
                    Width = rectangle.Right - rectangle.Left,
                    Height = rectangle.Bottom - rectangle.Top,
                    Style = GetWindowLongPtr(window, styleIndex).ToInt64(),
                    ExtendedStyle = GetWindowLongPtr(window, extendedStyleIndex).ToInt64()
                });
                return true;
            }, IntPtr.Zero);
            return result.ToArray();
        }
        finally
        {
            CloseDesktop(desktop);
            if (previousDpiContext != IntPtr.Zero)
                SetThreadDpiAwarenessContext(previousDpiContext);
        }
    }
}
'@

Add-Type -TypeDefinition $nativeSource -Language CSharp
[HiddenDesktopWindows]::Get($DesktopName, $ProcessId) |
    Sort-Object Width, Height -Descending
