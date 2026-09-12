#pragma once

// =============================================================================
//  memory.h — External memory read/write (port H-V2)
//  Dua handle dibuka sekali saat attach. Write di-gate writes_allowed (in-match).
//  Counter read/write untuk logger diagnostik.
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "../math/vec3.h"

#ifndef FORCEINLINE
#define FORCEINLINE __attribute__((always_inline)) inline
#endif
#ifndef LIKELY
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#endif

class Memory {
public:
    HANDLE    h_read      = nullptr;
    HANDLE    h_write     = nullptr;
    DWORD     process_id  = 0;
    uintptr_t client_base  = 0;
    uintptr_t engine2_base = 0;
    uint64_t  session = 0;
    bool      writes_allowed = false;
    mutable uint64_t read_calls = 0, read_failures = 0, write_failures = 0;

    bool Attach(const char* process_name) noexcept {
        Detach();
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (UNLIKELY(snap == INVALID_HANDLE_VALUE)) return false;
        PROCESSENTRY32 pe{};
        pe.dwSize = sizeof(pe);
        bool found = false;
        if (Process32First(snap, &pe)) {
            do {
                if (strcmp(pe.szExeFile, process_name) == 0) {
                    process_id = pe.th32ProcessID;
                    found = true;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
        if (UNLIKELY(!found)) return false;
        h_read = OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, process_id);
        if (!h_read) return false;
        h_write = OpenProcess(
            PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION,
            FALSE, process_id);
        ++session;
        return true;
    }

    void Detach() noexcept {
        if (h_read)  { CloseHandle(h_read);  h_read  = nullptr; }
        if (h_write) { CloseHandle(h_write); h_write = nullptr; }
        process_id  = 0;
        client_base  = 0;
        engine2_base = 0;
        writes_allowed = false;
    }

    [[nodiscard]] FORCEINLINE bool IsAlive() const noexcept {
        if (!h_read) return false;
        DWORD exit_code = 0;
        if (GetExitCodeProcess(h_read, &exit_code))
            return exit_code == STILL_ACTIVE;
        return false;
    }

    [[nodiscard]] FORCEINLINE bool CanWrite() const noexcept {
        return h_write != nullptr;
    }
    [[nodiscard]] FORCEINLINE bool CanModify() const noexcept {
        return CanWrite() && writes_allowed;
    }

    [[nodiscard]] static FORCEINLINE bool IsValidPtr(uintptr_t ptr) noexcept {
        return ptr >= 0x10000ULL && ptr < 0x7FFFFFFFFFFFULL;
    }

    template<typename T>
    [[nodiscard]] FORCEINLINE bool TryRead(uintptr_t addr, T& out) const noexcept {
        static_assert(std::is_trivially_copyable_v<T>);
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t v = 0;
            if (!ReadBlob(addr, &v, 1) || v > 1) { out = false; return false; }
            out = v != 0;
            return true;
        }
        T v{};
        if (!ReadBlob(addr, &v, sizeof v)) { out = T{}; return false; }
        out = v;
        return true;
    }

    template<typename T>
    [[nodiscard]] FORCEINLINE T Read(uintptr_t addr) const noexcept {
        T val{};
        (void)TryRead(addr, val);
        return val;
    }

    [[nodiscard]] FORCEINLINE Vec3 ReadVec3(uintptr_t addr) const noexcept {
        Vec3 val{};
        if (UNLIKELY(!h_read || !IsValidPtr(addr))) return val;
        ReadProcessMemory(h_read, reinterpret_cast<LPCVOID>(addr), &val, 12, nullptr);
        return val;
    }

    template<typename T>
    FORCEINLINE bool ReadArray(uintptr_t addr, T* buf, size_t count) const noexcept {
        if (UNLIKELY(!h_read || !buf || count == 0 || !IsValidPtr(addr))) return false;
        return ReadProcessMemory(h_read, reinterpret_cast<LPCVOID>(addr),
                                 buf, sizeof(T) * count, nullptr) != 0;
    }

    [[nodiscard]] FORCEINLINE bool ReadBlob(uintptr_t addr, void* dst, size_t size) const noexcept {
        ++read_calls;
        SIZE_T copied = 0;
        if (!h_read || !dst || !size || !IsValidPtr(addr) ||
            size > 0x7FFFFFFFFFFFULL - addr ||
            !ReadProcessMemory(h_read, reinterpret_cast<LPCVOID>(addr), dst, size, &copied) ||
            copied != size) {
            if (dst && size) memset(dst, 0, size);
            ++read_failures;
            return false;
        }
        return true;
    }

    template<typename T>
    FORCEINLINE bool Write(uintptr_t addr, const T& val) const noexcept {
        SIZE_T copied = 0;
        if (!CanModify() || !IsValidPtr(addr) || sizeof(T) > 0x7FFFFFFFFFFFULL - addr ||
            !WriteProcessMemory(h_write, reinterpret_cast<LPVOID>(addr),
                                &val, sizeof(T), &copied) || copied != sizeof(T)) {
            ++write_failures;
            return false;
        }
        return true;
    }

    FORCEINLINE bool WriteVec3(uintptr_t addr, const Vec3& val) const noexcept {
        if (UNLIKELY(!h_write || !IsValidPtr(addr))) return false;
        return WriteProcessMemory(h_write, reinterpret_cast<LPVOID>(addr),
                                  &val, 12, nullptr) != 0;
    }

    FORCEINLINE void ReadString(uintptr_t addr, char* out, size_t max_len) const noexcept {
        if (UNLIKELY(!out || max_len == 0)) return;
        out[0] = '\0';
        if (UNLIKELY(!h_read || !IsValidPtr(addr))) return;
        if (max_len > 128) max_len = 128;
        if (!ReadBlob(addr, out, max_len - 1)) return;
        out[max_len - 1] = '\0';
    }

    [[nodiscard]] uintptr_t GetModuleBase(const char* module_name) const noexcept {
        HANDLE snap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
        if (UNLIKELY(snap == INVALID_HANDLE_VALUE)) return 0;
        MODULEENTRY32 me{};
        me.dwSize = sizeof(me);
        uintptr_t base = 0;
        if (Module32First(snap, &me)) {
            do {
                if (strcmp(me.szModule, module_name) == 0) {
                    base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                    break;
                }
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        return base;
    }

    template<typename... Offsets>
    [[nodiscard]] FORCEINLINE uintptr_t ReadChain(uintptr_t base,
                                                   Offsets... offsets) const noexcept {
        uintptr_t addr = base;
        ((addr = Read<uintptr_t>(addr + offsets)), ...);
        return addr;
    }
};

inline Memory g_mem;
