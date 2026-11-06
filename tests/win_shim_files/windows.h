// Just enough of <windows.h> to compile src/core/builtin_files.cpp natively
// for tests/test_builtin_files.cpp, which implements the file calls over a
// fake folder.
#pragma once
#include <cstdint>

typedef unsigned long DWORD;
typedef int BOOL;
typedef const wchar_t* LPCWSTR;
typedef void* HANDLE;
typedef DWORD* LPDWORD;
struct SECURITY_ATTRIBUTES;
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<intptr_t>(-1)))
#define GENERIC_WRITE 0x40000000UL
#define CREATE_NEW 1
#define FILE_ATTRIBUTE_NORMAL 0x80
#define ERROR_FILE_EXISTS 80
#define ERROR_ACCESS_DENIED 5
BOOL CreateDirectoryW(LPCWSTR path, SECURITY_ATTRIBUTES* sa);
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* sa, DWORD disposition,
                   DWORD flags, HANDLE templ);
BOOL WriteFile(HANDLE h, const void* data, DWORD size, LPDWORD written, void* overlapped);
BOOL CloseHandle(HANDLE h);
BOOL DeleteFileW(LPCWSTR path);
DWORD GetLastError();
