// Just enough of <windows.h> to compile src/core/memory.cpp, src/ue/names.cpp
// and src/ue/objects.cpp natively for tests/test_objects.cpp. VirtualQuery
// answers from a fake memory map the test controls.
#pragma once
#include <cstddef>
#include <cstdint>

typedef unsigned long DWORD;
typedef unsigned short WORD;
typedef unsigned char BYTE;
typedef int BOOL;
typedef void* HMODULE;
typedef const wchar_t* LPCWSTR;
typedef size_t SIZE_T;

#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_WRITECOPY 0x08
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_EXECUTE_WRITECOPY 0x80
#define PAGE_GUARD 0x100

struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State;
    DWORD Protect;
    DWORD Type;
};

// Implemented by the test.
SIZE_T VirtualQuery(const void* p, MEMORY_BASIC_INFORMATION* mbi, SIZE_T len);
inline HMODULE GetModuleHandleW(LPCWSTR) { return nullptr; }

struct IMAGE_DOS_HEADER {
    long e_lfanew;
};
struct IMAGE_FILE_HEADER {
    WORD NumberOfSections;
};
struct IMAGE_OPTIONAL_HEADER64 {
    DWORD SizeOfImage;
};
struct IMAGE_NT_HEADERS64 {
    IMAGE_FILE_HEADER FileHeader;
    IMAGE_OPTIONAL_HEADER64 OptionalHeader;
};
struct IMAGE_SECTION_HEADER {
    BYTE Name[8];
    union {
        DWORD VirtualSize;
    } Misc;
    DWORD VirtualAddress;
    DWORD SizeOfRawData;
};
#define IMAGE_FIRST_SECTION(nt) (reinterpret_cast<IMAGE_SECTION_HEADER*>((nt) + 1))
