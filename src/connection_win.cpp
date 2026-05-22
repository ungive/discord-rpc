#include "connection.h"

#define WIN32_LEAN_AND_MEAN
#define NOMCX
#define NOSERVICE
#define NOIME
#include <assert.h>
#include <windows.h>

int GetProcessId()
{
    return (int)::GetCurrentProcessId();
}

struct BaseConnectionWin : public BaseConnection {
    HANDLE pipe{INVALID_HANDLE_VALUE};
    std::string path;
};

/*static*/ BaseConnection* BaseConnection::Create(const char* path)
{
    auto* c = new BaseConnectionWin();
    c->path = path;
    return c;
}

/*static*/ void BaseConnection::Destroy(BaseConnection*& c)
{
    auto self = reinterpret_cast<BaseConnectionWin*>(c);
    self->Close();
    delete self;
    c = nullptr;
}

/*static*/ std::vector<std::string> BaseConnection::ScanAvailablePaths()
{
    std::vector<std::string> paths;
    WIN32_FIND_DATAW findData;
    HANDLE hFind = ::FindFirstFileW(L"\\\\.\\pipe\\discord-ipc-*", &findData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return paths;
    }
    do {
        // Accept only "discord-ipc-" + single digit (0-9)
        const wchar_t* prefix = L"discord-ipc-";
        const size_t prefixLen = 12;
        const wchar_t* name = findData.cFileName;
        if (wcsncmp(name, prefix, prefixLen) == 0 && name[prefixLen] >= L'0' &&
            name[prefixLen] <= L'9' && name[prefixLen + 1] == L'\0') {
            char narrowPath[64];
            snprintf(narrowPath, sizeof(narrowPath), "\\\\.\\pipe\\discord-ipc-%c", (char)name[prefixLen]);
            paths.emplace_back(narrowPath);
        }
    } while (::FindNextFileW(hFind, &findData));
    ::FindClose(hFind);
    return paths;
}

bool BaseConnection::Open()
{
    auto self = reinterpret_cast<BaseConnectionWin*>(this);
    wchar_t wPath[256];
    ::MultiByteToWideChar(CP_ACP, 0, self->path.c_str(), -1, wPath, 256);
    for (;;) {
        self->pipe = ::CreateFileW(
          wPath, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (self->pipe != INVALID_HANDLE_VALUE) {
            self->isOpen = true;
            return true;
        }
        auto lastError = GetLastError();
        if (lastError == ERROR_PIPE_BUSY) {
            if (!WaitNamedPipeW(wPath, 10000)) {
                return false;
            }
            continue;
        }
        return false;
    }
}

bool BaseConnection::Close()
{
    auto self = reinterpret_cast<BaseConnectionWin*>(this);
    ::CloseHandle(self->pipe);
    self->pipe = INVALID_HANDLE_VALUE;
    self->isOpen = false;
    return true;
}

bool BaseConnection::Write(const void* data, size_t length)
{
    if (length == 0) {
        return true;
    }
    auto self = reinterpret_cast<BaseConnectionWin*>(this);
    assert(self);
    if (!self) {
        return false;
    }
    if (self->pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    assert(data);
    if (!data) {
        return false;
    }
    const DWORD bytesLength = (DWORD)length;
    DWORD bytesWritten = 0;
    return ::WriteFile(self->pipe, data, bytesLength, &bytesWritten, nullptr) == TRUE &&
      bytesWritten == bytesLength;
}

bool BaseConnection::Read(void* data, size_t length)
{
    assert(data);
    if (!data) {
        return false;
    }
    auto self = reinterpret_cast<BaseConnectionWin*>(this);
    assert(self);
    if (!self) {
        return false;
    }
    if (self->pipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD bytesAvailable = 0;
    if (::PeekNamedPipe(self->pipe, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
        if (bytesAvailable >= length) {
            DWORD bytesToRead = (DWORD)length;
            DWORD bytesRead = 0;
            if (::ReadFile(self->pipe, data, bytesToRead, &bytesRead, nullptr) == TRUE) {
                assert(bytesToRead == bytesRead);
                return true;
            }
            else {
                Close();
            }
        }
    }
    else {
        Close();
    }
    return false;
}
