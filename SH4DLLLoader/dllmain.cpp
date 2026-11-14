// SH4 DLL Loader (dinput8.dll) - mod loader and DirectInput 8 proxy for
// Silent Hill 4 (PC).
//
// DirectInput8Create goes to dinput8_original.dll (the game's own dinput8,
// renamed) or, without it, to the Windows dinput8.dll.
//
// Mods: every 32-bit *.dll in the game folder that exports Initialize and is
// not already loaded (.NET DLLs first, then alphabetical). They are loaded
// when the game reaches its entry point, which DllMain patches; if that
// patch fails, no mod is loaded (the game runs without them). Each
// Initialize() is waited for, so the mods' hooks are in place before the
// game's code runs. If an Initialize() crashes, the game is closed (its
// hooks may be half installed).
// A mod's DllMain and the threads it starts are its own responsibility.
//
// Log: SH4CustomModules.log next to the exe, shared with the mods.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    // ======================================================================
    // Paths
    // ======================================================================

    std::wstring DirOf(const std::wstring& path)
    {
        const size_t pos = path.find_last_of(L"\\/");
        return pos == std::wstring::npos ? L"." : path.substr(0, pos);
    }

    std::wstring ModulePath(HMODULE module)
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(module, path, static_cast<DWORD>(_countof(path)));
        return path;
    }

    std::wstring SelfDir()
    {
        return DirOf(ModulePath(reinterpret_cast<HMODULE>(&__ImageBase)));
    }

    std::wstring Lower(std::wstring s)
    {
        for (auto& c : s)
            c = static_cast<wchar_t>(std::towlower(c));
        return s;
    }

    std::string Narrow(const std::wstring& s)
    {
        if (s.empty())
            return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &out[0], n, nullptr, nullptr);
        return out;
    }

    // ======================================================================
    // Log (shared SH4CustomModules.log, same kernel object names as the mods)
    // ======================================================================

    HANDLE g_logMutex = nullptr;
    HANDLE g_logSession = nullptr;
    std::wstring g_logPath;

    std::wstring KernelName(const wchar_t* prefix)
    {
        wchar_t name[128] = {};
        _snwprintf_s(name, _countof(name), _TRUNCATE, L"Local\\%ls_%lu", prefix,
                     static_cast<unsigned long>(GetCurrentProcessId()));
        return name;
    }

    void WriteLine(FILE* f, const char* text)
    {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        fprintf(f, "[%02u:%02u:%02u.%03u][SH4DLLLoader] %s\n",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text);
    }

    void Log(const char* fmt, ...)
    {
        char text[1024] = {};
        va_list ap;
        va_start(ap, fmt);
        _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, ap);
        va_end(ap);

        if (g_logPath.empty())
            g_logPath = DirOf(ModulePath(nullptr)) + L"\\SH4CustomModules.log";

        if (!g_logMutex)
            g_logMutex = CreateMutexW(nullptr, FALSE, KernelName(L"SH4CustomModulesLogMutex").c_str());

        const DWORD wait = g_logMutex ? WaitForSingleObject(g_logMutex, 2000) : WAIT_FAILED;
        const bool locked = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;

        // The first module of the session (normally this loader) starts the file.
        const wchar_t* mode = L"a";
        if (!g_logSession)
        {
            SetLastError(ERROR_SUCCESS);
            g_logSession = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1,
                                              KernelName(L"SH4CustomModulesLogSession").c_str());
            if (g_logSession && GetLastError() != ERROR_ALREADY_EXISTS)
                mode = L"w";
        }

        FILE* f = nullptr;
        if (_wfopen_s(&f, g_logPath.c_str(), mode) == 0 && f)
        {
            if (mode[0] == L'w')
            {
                char first[128] = {};
                _snprintf_s(first, sizeof(first), _TRUNCATE,
                            "DLL log initialized | PID=%lu | first module in this SH4 session",
                            static_cast<unsigned long>(GetCurrentProcessId()));
                WriteLine(f, first);
            }
            WriteLine(f, text);
            fclose(f);
        }

        if (locked)
            ReleaseMutex(g_logMutex);
    }

    constexpr DWORD kSlowInitLogMs = 5000; // "still running" log interval

    // Closes the game: continuing would risk a crash later.
    [[noreturn]] void Fatal(const wchar_t* text)
    {
        MessageBoxW(nullptr, text, L"SH4 DLL Loader", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }

    // ======================================================================
    // Original dinput8
    // ======================================================================

    HMODULE g_original = nullptr;
    INIT_ONCE g_originalOnce = INIT_ONCE_STATIC_INIT;

    BOOL CALLBACK LoadOriginalOnce(PINIT_ONCE, PVOID, PVOID*)
    {
        const std::wstring local = SelfDir() + L"\\dinput8_original.dll";
        g_original = LoadLibraryW(local.c_str());
        if (g_original)
        {
            Log("dinput8_original.dll loaded");
            return TRUE;
        }

        const DWORD localError = GetLastError();
        wchar_t sys[MAX_PATH] = {};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            g_original = LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());

        if (g_original)
            Log("dinput8_original.dll not loaded (error %lu); using the Windows dinput8.dll", localError);
        else
            Log("ERROR: no dinput8 implementation could be loaded (error %lu)", GetLastError());
        return g_original != nullptr;
    }

    bool LoadOriginal()
    {
        return InitOnceExecuteOnce(&g_originalOnce, LoadOriginalOnce, nullptr, nullptr) && g_original;
    }

    // ======================================================================
    // Audio fix (the game's audio code
    // ======================================================================

    struct BytePatch
    {
        uintptr_t address;
        uint8_t original[4];
        uint8_t patched[4];
        size_t size;
    };

    const BytePatch kAudioFix[] = {
        { 0x0040904B, { 0x90, 0x90 }, { 0x75, 0x43 }, 2 },
        { 0x0040B59C, { 0x90, 0x90 }, { 0x75, 0x41 }, 2 },
    };

    bool WriteCode(uintptr_t address, const void* data, size_t size)
    {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old))
            return false;
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        DWORD tmp = 0;
        VirtualProtect(reinterpret_cast<void*>(address), size, old, &tmp);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
        return true;
    }

    void ApplyAudioFix()
    {
        for (const auto& p : kAudioFix)
        {
            if (std::memcmp(reinterpret_cast<const void*>(p.address), p.original, p.size) != 0)
            {
                Log("AudioFix: SKIPPED - the bytes at 0x%08X are not the expected ones (nothing written)",
                    static_cast<unsigned>(p.address));
                return;
            }
        }

        for (const auto& p : kAudioFix)
        {
            if (!WriteCode(p.address, p.patched, p.size) ||
                std::memcmp(reinterpret_cast<const void*>(p.address), p.patched, p.size) != 0)
            {
                Log("AudioFix: ERROR writing 0x%08X", static_cast<unsigned>(p.address));
                return;
            }
        }
        Log("AudioFix: applied");
    }

    // ======================================================================
    // Mod discovery (on disk, nothing executed)
    // ======================================================================

    // Reads the file's headers and export names; true if it is a 32-bit DLL
    // exporting "Initialize".
    bool IsModFile(const std::wstring& path, bool& managed)
    {
        managed = false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        const uint8_t* base = mapping
            ? static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0))
            : nullptr;

        LARGE_INTEGER size = {};
        GetFileSizeEx(file, &size);
        const uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);

        bool found = false;

        __try
        {
            auto inFile = [&](uint64_t off, uint64_t len) { return off + len <= fileSize; };

            if (base && inFile(0, sizeof(IMAGE_DOS_HEADER)))
            {
                const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
                if (dos->e_magic == IMAGE_DOS_SIGNATURE &&
                    inFile(static_cast<uint64_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS32)))
                {
                    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
                    if (nt->Signature == IMAGE_NT_SIGNATURE &&
                        nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
                        (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0 &&
                        nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
                        nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT)
                    {
                        const auto* sections = IMAGE_FIRST_SECTION(nt);
                        const WORD count = nt->FileHeader.NumberOfSections;

                        auto rvaToOff = [&](DWORD rva, uint64_t& off) {
                            for (WORD i = 0; i < count; ++i)
                            {
                                const auto& s = sections[i];
                                const DWORD span = s.SizeOfRawData > s.Misc.VirtualSize
                                    ? s.SizeOfRawData : s.Misc.VirtualSize;
                                if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span)
                                {
                                    off = static_cast<uint64_t>(rva - s.VirtualAddress) + s.PointerToRawData;
                                    return true;
                                }
                            }
                            return false;
                        };

                        managed =
                            nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR &&
                            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0;

                        const DWORD expRva =
                            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
                        uint64_t expOff = 0;
                        if (expRva && rvaToOff(expRva, expOff) && inFile(expOff, sizeof(IMAGE_EXPORT_DIRECTORY)))
                        {
                            const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + expOff);
                            uint64_t namesOff = 0;
                            if (exp->NumberOfNames && rvaToOff(exp->AddressOfNames, namesOff) &&
                                inFile(namesOff, static_cast<uint64_t>(exp->NumberOfNames) * 4))
                            {
                                const auto* names = reinterpret_cast<const DWORD*>(base + namesOff);
                                for (DWORD i = 0; i < exp->NumberOfNames && !found; ++i)
                                {
                                    uint64_t nameOff = 0;
                                    if (rvaToOff(names[i], nameOff) && inFile(nameOff, 11) &&
                                        std::memcmp(base + nameOff, "Initialize", 11) == 0)
                                    {
                                        found = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            found = false;
        }

        if (base)
            UnmapViewOfFile(base);
        if (mapping)
            CloseHandle(mapping);
        CloseHandle(file);
        return found;
    }

    struct ModFile
    {
        std::wstring name;  // as on disk
        std::wstring lower; // lower-case, for ordering / matching
        std::wstring path;
        bool managed = false;
    };

    std::vector<ModFile> FindMods()
    {
        std::vector<ModFile> mods;
        const std::wstring dir = SelfDir();

        WIN32_FIND_DATAW fd = {};
        HANDLE find = FindFirstFileW((dir + L"\\*.dll").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
            return mods;

        do
        {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;

            ModFile m;
            m.name = fd.cFileName;
            m.lower = Lower(m.name);
            m.path = dir + L"\\" + m.name;

            // Already in the process: the game's own imports and their
            // wrappers, dinput8_original.dll, this DLL.
            if (GetModuleHandleW(m.path.c_str()) || GetModuleHandleW(m.name.c_str()))
                continue;

            if (IsModFile(m.path, m.managed))
                mods.push_back(m);
        } while (FindNextFileW(find, &fd));

        FindClose(find);

        // .NET DLLs first, then alphabetical.
        std::sort(mods.begin(), mods.end(), [](const ModFile& a, const ModFile& b) {
            return a.managed != b.managed ? a.managed : a.lower < b.lower;
        });
        return mods;
    }

    // ======================================================================
    // Loading
    // ======================================================================

    using InitFn = void(__cdecl*)();

    struct InitCall
    {
        InitFn fn;
        DWORD exceptionCode;
        volatile LONG done; // set last by the worker thread
    };

    int KeepCode(DWORD code, DWORD* out)
    {
        *out = code;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    DWORD WINAPI InitThread(LPVOID param)
    {
        auto* call = static_cast<InitCall*>(param);
        __try
        {
            call->fn();
        }
        __except (KeepCode(GetExceptionCode(), &call->exceptionCode))
        {
        }
        InterlockedExchange(&call->done, 1); // last use of 'call'
        return 0;
    }

    void LoadMod(const ModFile& mod)
    {
        const std::string name = Narrow(mod.name);
        const DWORD start = GetTickCount();

        HMODULE module = LoadLibraryExW(mod.path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!module)
        {
            Log("%s: FAILED to load (error %lu)", name.c_str(), GetLastError());
            return;
        }

        const auto init = reinterpret_cast<InitFn>(GetProcAddress(module, "Initialize"));
        if (!init)
        {
            Log("%s: loaded (no Initialize)", name.c_str());
            return;
        }

        // Waited for without a time limit: the game must not start while a
        // mod is still installing hooks. A worker thread only so a slow mod
        // shows up in the log.
        InitCall call = { init, 0, 0 };
        HANDLE thread = CreateThread(nullptr, 0, InitThread, &call, 0, nullptr);
        if (thread)
        {
            // 'call' lives on this stack: leave only once the thread has
            // ended or is done with it. If the wait itself fails, the flag
            // is polled instead.
            DWORD lastLog = GetTickCount();
            for (;;)
            {
                const DWORD wait = WaitForSingleObject(thread, kSlowInitLogMs);
                if (wait == WAIT_OBJECT_0 || call.done)
                    break;
                if (wait == WAIT_FAILED)
                    Sleep(50);
                if (GetTickCount() - lastLog >= kSlowInitLogMs)
                {
                    lastLog = GetTickCount();
                    Log("%s: Initialize() still running after %lu ms; waiting (the game starts after it)",
                        name.c_str(), static_cast<unsigned long>(lastLog - start));
                }
            }
            CloseHandle(thread);
        }
        else
        {
            InitThread(&call);
        }

        // An exception, or the thread ended inside Initialize() (ExitThread):
        // what it installed so far stays in place and cannot be undone here.
        if (call.exceptionCode || !call.done)
        {
            Log("%s: Initialize() CRASHED (exception 0x%08lX); closing the game", name.c_str(),
                static_cast<unsigned long>(call.exceptionCode));
            wchar_t text[512] = {};
            _snwprintf_s(text, _countof(text), _TRUNCATE,
                         L"%ls crashed while starting (exception 0x%08lX).\n\n"
                         L"The game was closed so it does not crash later. "
                         L"Remove or update this mod.\n\nDetails: SH4CustomModules.log",
                         mod.name.c_str(), static_cast<unsigned long>(call.exceptionCode));
            Fatal(text);
        }
        Log("%s: loaded + Initialize() OK (%lu ms)", name.c_str(),
            static_cast<unsigned long>(GetTickCount() - start));
    }

    void LoadEverything(const char* when)
    {
        Log("loader start (%s)", when);

        LoadOriginal();
        ApplyAudioFix();

        const std::vector<ModFile> mods = FindMods();
        Log("%u mod(s) found (32-bit DLLs exporting Initialize)", static_cast<unsigned>(mods.size()));

        for (const auto& mod : mods)
            LoadMod(mod);

        Log("loader done");
    }

    // ======================================================================
    // Game entry point hook
    // ======================================================================

    uintptr_t g_entry = 0;
    uint8_t g_entryBytes[5] = {};

    // Runs in place of the game's entry point (mainCRTStartup, reached with a
    // jump): puts the entry bytes back, loads the mods, then calls the real
    // entry point (it never returns: the game ends with ExitProcess).
    DWORD WINAPI EntryHook(void* arg)
    {
        void* entry = reinterpret_cast<void*>(g_entry);
        if (!WriteCode(g_entry, g_entryBytes, sizeof(g_entryBytes)))
            WriteProcessMemory(GetCurrentProcess(), entry, g_entryBytes, sizeof(g_entryBytes), nullptr);

        if (std::memcmp(entry, g_entryBytes, sizeof(g_entryBytes)) != 0)
        {
            // Calling it now would jump back here.
            Log("ERROR: the game's entry point could not be restored (error %lu); closing the game",
                GetLastError());
            Fatal(L"The game's entry point could not be restored.\n\nDetails: SH4CustomModules.log");
        }

        LoadEverything("before the game's entry point");
        return reinterpret_cast<DWORD(__cdecl*)(void*)>(g_entry)(arg);
    }

    bool HookGameEntry()
    {
        const auto exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(exe + dos->e_lfanew);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
            !nt->OptionalHeader.AddressOfEntryPoint)
        {
            return false;
        }

        g_entry = exe + nt->OptionalHeader.AddressOfEntryPoint;
        std::memcpy(g_entryBytes, reinterpret_cast<const void*>(g_entry), sizeof(g_entryBytes));

        uint8_t jump[5] = { 0xE9 };
        const int32_t rel = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(&EntryHook) - (g_entry + 5));
        std::memcpy(jump + 1, &rel, 4);
        return WriteCode(g_entry, jump, sizeof(jump));
    }

    // Set in DllMain when the entry point could not be patched; logged on
    // the first DirectInput8Create (no file writes under the loader lock).
    volatile LONG g_noMods = 0;
}

// ==========================================================================
// dinput8 exports
// ==========================================================================

typedef HRESULT(WINAPI* DirectInput8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);

extern "C" HRESULT WINAPI DirectInput8Create(
    HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    if (InterlockedCompareExchange(&g_noMods, 2, 1) == 1)
        Log("ERROR: the game's entry point could not be patched; no mod and no audio fix were "
            "loaded (loading them while the game runs could crash it)");

    if (!LoadOriginal())
        return E_FAIL;

    const auto real = reinterpret_cast<DirectInput8Create_t>(GetProcAddress(g_original, "DirectInput8Create"));
    if (!real)
    {
        Log("ERROR: DirectInput8Create not found in the original dinput8");
        return E_FAIL;
    }
    return real(hinst, version, riid, out, outer);
}

HRESULT WINAPI DllCanUnloadNow(void) { return S_OK; }
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID*) { return CLASS_E_CLASSNOTAVAILABLE; }
HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }

// ==========================================================================
// DllMain
// ==========================================================================

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        // Nothing is loaded here (loader lock): the mods are loaded when the
        // game reaches its entry point. Without that patch, none are loaded.
        if (!HookGameEntry())
            g_noMods = 1;
    }
    return TRUE;
}
