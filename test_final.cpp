#include <iostream>
#include <windows.h>
#include <string>

int main() {
    std::wstring wpath = L"N:\\Users\\shao\\acedesktoptest";
    HANDLE h = ::CreateFileW(
        wpath.c_str(),
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::cout << "CreateFileW failed " << GetLastError() << "\n";
        return 1;
    }
    std::wstring buffer(1024, L'\0');
    DWORD written = ::GetFinalPathNameByHandleW(
        h, buffer.data(), 1024,
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    ::CloseHandle(h);
    if (written == 0) {
        std::cout << "GetFinalPathNameByHandleW failed " << GetLastError() << "\n";
        return 1;
    }
    buffer.resize(written);
    std::wcout << L"Final path: " << buffer << L"\n";
    return 0;
}
