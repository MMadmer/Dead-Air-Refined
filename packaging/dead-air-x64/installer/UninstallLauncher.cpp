#include <windows.h>
#include <shellapi.h>

#include <filesystem>
#include <string>
#include <vector>

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t* commandLine, int)
{
    std::vector<wchar_t> executablePath(32768);
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
    if (!pathLength || pathLength == executablePath.size())
    {
        MessageBoxW(nullptr, L"Не удалось определить папку игры.", L"Dead Air: Refined", MB_OK | MB_ICONERROR);
        return 1;
    }

    const std::filesystem::path uninstaller =
        std::filesystem::path(std::wstring(executablePath.data(), pathLength)).parent_path() /
        L".dead-air-x64" / L"unins000.exe";
    if (!std::filesystem::is_regular_file(uninstaller))
    {
        MessageBoxW(
            nullptr, L"Встроенный деинсталлятор Dead Air: Refined не найден.", L"Dead Air: Refined", MB_OK | MB_ICONERROR);
        return 2;
    }

    SHELLEXECUTEINFOW executeInfo{};
    executeInfo.cbSize = sizeof(executeInfo);
    executeInfo.fMask = SEE_MASK_FLAG_NO_UI;
    executeInfo.hwnd = nullptr;
    executeInfo.lpVerb = L"open";
    const std::wstring uninstallerPath = uninstaller.wstring();
    executeInfo.lpFile = uninstallerPath.c_str();
    executeInfo.lpParameters = commandLine;
    executeInfo.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&executeInfo))
    {
        MessageBoxW(
            nullptr, L"Не удалось запустить деинсталлятор Dead Air: Refined.", L"Dead Air: Refined", MB_OK | MB_ICONERROR);
        return 3;
    }

    return 0;
}
