#include <string>
#include <windows.h>

namespace Mashiro {

    void SetCurrentThreadName(const char* iName) {
        if (!iName) {
            return;
        }

        // Convert to wide string
        int len = MultiByteToWideChar(CP_UTF8, 0, iName, -1, nullptr, 0);
        if (len <= 0) {
            return;
        }

        std::wstring wname(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, iName, -1, wname.data(), len);

        // Use SetThreadDescription (Windows 10 1607+)
        // This is the modern API that works with debuggers and ETW
        SetThreadDescription(GetCurrentThread(), wname.c_str());
    }

    void SetThreadName(void* iNativeHandle, const char* iName) {
        if (!iNativeHandle || !iName) {
            return;
        }

        int len = MultiByteToWideChar(CP_UTF8, 0, iName, -1, nullptr, 0);
        if (len <= 0) {
            return;
        }

        std::wstring wname(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, iName, -1, wname.data(), len);

        SetThreadDescription(iNativeHandle, wname.c_str());
    }

    std::string GetCurrentThreadName() {
        PWSTR wname = nullptr;
        HRESULT hr = GetThreadDescription(GetCurrentThread(), &wname);
        if (FAILED(hr) || !wname) {
            return "";
        }

        // Convert wide string to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, wname, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) {
            LocalFree(wname);
            return "";
        }

        std::string result(len - 1, '\0'); // -1 to exclude null terminator
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, result.data(), len, nullptr, nullptr);
        LocalFree(wname);
        return result;
    }

    std::string GetThreadName(void* iNativeHandle) {
        if (!iNativeHandle) {
            return "";
        }

        PWSTR wname = nullptr;
        HRESULT hr = GetThreadDescription(iNativeHandle, &wname);
        if (FAILED(hr) || !wname) {
            return "";
        }

        // Convert wide string to UTF-8
        int len = WideCharToMultiByte(CP_UTF8, 0, wname, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 0) {
            LocalFree(wname);
            return "";
        }

        std::string result(len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, result.data(), len, nullptr, nullptr);
        LocalFree(wname);
        return result;
    }

} // namespace Mashiro
