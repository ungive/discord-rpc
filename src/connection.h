#pragma once

// This is to wrap the platform specific kinds of connect/read/write.

#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <vector>

// not really connectiony, but need per-platform
int GetProcessId();

struct BaseConnection {
    static BaseConnection* Create(const char* path);
    static void Destroy(BaseConnection*&);
    // Return all currently available Discord IPC socket/pipe paths.
    static std::vector<std::string> ScanAvailablePaths();
    bool isOpen{false};
    bool Open();
    bool Close();
    bool Write(const void* data, size_t length);
    bool Read(void* data, size_t length);
    const char* Path() const;
};
