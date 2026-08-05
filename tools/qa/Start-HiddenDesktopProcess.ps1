param(
    [Parameter(Mandatory)]
    [string]$FilePath,

    [string]$Arguments = '',

    [Parameter(Mandatory)]
    [string]$ProcessIdFile,

    [string]$DesktopName = 'DeadAirQA',

    [ValidateRange(1, 3600)]
    [int]$TimeoutSeconds = 180,

    [string]$ExitCodeFile
)

$ErrorActionPreference = 'Stop'

$nativeSource = @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Threading;

public static class HiddenDesktopProcess
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

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateDesktop(
        string desktop,
        IntPtr device,
        IntPtr deviceMode,
        int flags,
        int desiredAccess,
        IntPtr securityAttributes);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcess(
        string applicationName,
        string commandLine,
        IntPtr processAttributes,
        IntPtr threadAttributes,
        bool inheritHandles,
        int creationFlags,
        IntPtr environment,
        string currentDirectory,
        ref STARTUPINFO startupInfo,
        out PROCESS_INFORMATION processInformation);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern int WaitForSingleObject(IntPtr handle, int milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateProcess(IntPtr process, int exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

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

    private static void FocusProcessWindow(IntPtr desktop, int processId)
    {
        var focusThread = new Thread(() =>
        {
            if (!SetThreadDesktop(desktop))
                return;

            for (int attempt = 0; attempt < 150; ++attempt)
            {
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

                if (target != IntPtr.Zero)
                {
                    SetForegroundWindow(target);
                    SetFocus(target);
                    return;
                }

                Thread.Sleep(100);
            }
        });
        focusThread.IsBackground = true;
        focusThread.Start();
        focusThread.Join(16000);
    }

    public static int Run(
        string filePath,
        string arguments,
        string desktopName,
        int timeoutSeconds,
        Action<int> onStarted,
        Action<uint> onExited)
    {
        const int desktopAllAccess = 0x01FF;
        const int createNewProcessGroup = 0x00000200;
        const int createNoWindow = 0x08000000;
        const int waitObject = 0;
        const int waitTimeout = 258;

        IntPtr desktop = CreateDesktop(desktopName, IntPtr.Zero, IntPtr.Zero, 0, desktopAllAccess, IntPtr.Zero);
        if (desktop == IntPtr.Zero)
            throw new Win32Exception(Marshal.GetLastWin32Error());

        var startupInfo = new STARTUPINFO
        {
            cb = Marshal.SizeOf<STARTUPINFO>(),
            lpDesktop = @"WinSta0\" + desktopName
        };

        PROCESS_INFORMATION processInformation;
        string commandLine = "\"" + filePath + "\" " + arguments;
        bool started = CreateProcess(
            filePath,
            commandLine,
            IntPtr.Zero,
            IntPtr.Zero,
            false,
            createNewProcessGroup | createNoWindow,
            IntPtr.Zero,
            System.IO.Path.GetDirectoryName(filePath),
            ref startupInfo,
            out processInformation);

        if (!started)
        {
            int error = Marshal.GetLastWin32Error();
            CloseDesktop(desktop);
            throw new Win32Exception(error);
        }

        try
        {
            CloseHandle(processInformation.hThread);
            processInformation.hThread = IntPtr.Zero;
            onStarted(processInformation.dwProcessId);
            FocusProcessWindow(desktop, processInformation.dwProcessId);

            int waitResult = WaitForSingleObject(processInformation.hProcess, checked(timeoutSeconds * 1000));
            if (waitResult == waitTimeout)
            {
                if (!TerminateProcess(processInformation.hProcess, 1))
                    throw new Win32Exception(Marshal.GetLastWin32Error());

                waitResult = WaitForSingleObject(processInformation.hProcess, 10000);
            }

            if (waitResult != waitObject)
                throw new Win32Exception(Marshal.GetLastWin32Error());

            if (!GetExitCodeProcess(processInformation.hProcess, out uint exitCode))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            onExited(exitCode);

            return processInformation.dwProcessId;
        }
        finally
        {
            if (processInformation.hThread != IntPtr.Zero)
                CloseHandle(processInformation.hThread);

            CloseHandle(processInformation.hProcess);
            CloseDesktop(desktop);
        }
    }
}
'@

Add-Type -TypeDefinition $nativeSource -Language CSharp

[HiddenDesktopProcess]::Run(
    $FilePath,
    $Arguments,
    $DesktopName,
    $TimeoutSeconds,
    {
        param($processId)
        Set-Content -LiteralPath $ProcessIdFile -Value $processId -Encoding ascii
    },
    {
        param($exitCode)
        if ($ExitCodeFile) {
            Set-Content -LiteralPath $ExitCodeFile -Value $exitCode -Encoding ascii
        }
    }
)
