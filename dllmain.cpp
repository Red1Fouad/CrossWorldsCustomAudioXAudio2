#define NOMINMAX
#include <windows.h>
#define DR_MP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#include <thread>
#include <atomic>
#include <iostream>
#include <cstdio>
#include <sstream>
#include <vector>
#include <psapi.h>
#include <map>
#include <mutex>
#include <random>
#include "AudioEngine.h"
#include "MinHook.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "libMinHook-x64-v141-md.lib")

std::atomic<bool> bIsRunning{ false };
HMODULE g_hModule = nullptr;

std::map<void*, std::string> g_playerCueNames;
std::map<void*, int> g_playerCategoryIds;
std::map<void*, std::string> g_acbFiles;
std::map<void*, std::string> g_playerAcbFiles;
std::mutex g_playerMutex;

AudioEngine* g_audio = nullptr;
std::vector<std::string> g_soundPaths;
std::vector<WavData> g_preloadedSounds;
std::atomic<bool> g_playedFinish{ false };
bool g_playNewMusic = false;
bool g_muteOnUnfocus = false;
float g_customBgmVolume = 1.0f;
float g_originalBgmVolume = 1.0f;
int g_lastPlayedIndex = -1;
std::mt19937 g_rng(std::random_device{}());

std::vector<int> ParseSignature(const std::string& signature) {
    std::vector<int> bytes;
    std::stringstream ss(signature);
    std::string byteStr;
    while (ss >> byteStr) {
        if (byteStr == "??" || byteStr == "?") bytes.push_back(-1);
        else bytes.push_back(std::stoi(byteStr, nullptr, 16));
    }
    return bytes;
}

uintptr_t PatternScan(HMODULE hModule, const std::string& signature) {
    if (!hModule) return 0;
    MODULEINFO modInfo;
    if (!GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(MODULEINFO))) return 0;

    uint8_t* startAddr = (uint8_t*)modInfo.lpBaseOfDll;
    size_t size = modInfo.SizeOfImage;
    std::vector<int> pattern = ParseSignature(signature);

    for (size_t i = 0; i < size - pattern.size(); ++i) {
        bool found = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] != -1 && startAddr[i + j] != pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) return (uintptr_t)(startAddr + i);
    }
    return 0;
}

bool SafeReadString(const char* src, char* dest, size_t maxLen) {
    __try {
        if (!src) return false;
        for (size_t i = 0; i < maxLen; ++i) {
            dest[i] = src[i];
            if (src[i] == '\0') return true;
        }
        dest[maxLen - 1] = '\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

typedef void (*criAtomExPlayer_SetCueName_t)(void* player, void* acb, const char* cueName);
criAtomExPlayer_SetCueName_t fpSetCueNameOriginal = nullptr;

void Hook_SetCueName(void* player, void* acb, const char* cueName) {
    char safeName[256];
    if (SafeReadString(cueName, safeName, sizeof(safeName))) {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        g_playerCueNames[player] = safeName;
        if (g_acbFiles.count(acb))
            g_playerAcbFiles[player] = g_acbFiles[acb];
        else
            g_playerAcbFiles[player] = "Unknown (Memory/Data)";
        if (strncmp(safeName, "BGM_", 4) == 0 || strncmp(safeName, "SE_FINISH", 9) == 0)
            std::cout << "[Mod] CRI SetCueName: " << safeName << std::endl;
    }
    if (fpSetCueNameOriginal) fpSetCueNameOriginal(player, acb, cueName);
}

typedef void (*criAtomExPlayer_SetCategoryById_t)(void* player, int id);
criAtomExPlayer_SetCategoryById_t fpSetCategoryByIdOriginal = nullptr;

void Hook_SetCategoryById(void* player, int id) {
    {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        g_playerCategoryIds[player] = id;
    }
    if (fpSetCategoryByIdOriginal) fpSetCategoryByIdOriginal(player, id);
}

typedef void* (*criAtomExAcb_LoadAcbFile_t)(void* acbLib, const char* path, void* work, int workSize, void* buff, int buffSize);
criAtomExAcb_LoadAcbFile_t fpLoadAcbFileOriginal = nullptr;

void* Hook_LoadAcbFile(void* acbLib, const char* path, void* work, int workSize, void* buff, int buffSize) {
    void* ret = fpLoadAcbFileOriginal(acbLib, path, work, workSize, buff, buffSize);
    if (ret && path) {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        g_acbFiles[ret] = path;
    }
    return ret;
}

// Direct function addresses (not hooked)
typedef void (*criAtomExCategory_SetVolume_t)(int id, float vol);
criAtomExCategory_SetVolume_t fpCategorySetVolume = nullptr;
typedef float (*criAtomExCategory_GetVolume_t)(int id);
criAtomExCategory_GetVolume_t fpCategoryGetVolume = nullptr;

typedef uint32_t (*criAtomExPlayer_Start_t)(void* player);
criAtomExPlayer_Start_t fpStartOriginal = nullptr;

uint32_t Hook_Start(void* player) {
    std::string name = "Unknown";
    std::string acbFile = "";
    int catId = -1;
    {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        if (g_playerCueNames.count(player))
            name = g_playerCueNames[player];
        if (g_playerCategoryIds.count(player))
            catId = g_playerCategoryIds[player];
        if (g_playerAcbFiles.count(player))
            acbFile = g_playerAcbFiles[player];
    }

    if (name.find("SE_FINISH") == 0 || name.find("BGM_") == 0) {
        std::stringstream ss;
        ss << "[Mod] CRI Start -> Cue: " << name;
        if (!acbFile.empty()) ss << " | ACB: " << acbFile;
        if (fpCategoryGetVolume) {
            if (catId != -1) ss << " | PlayerCat(" << catId << "): " << fpCategoryGetVolume(catId);
            ss << " | Cats:";
            for (int i = 0; i < 16; ++i) ss << " [" << i << "]:" << fpCategoryGetVolume(i);
        }
        std::cout << ss.str() << std::endl;
    }

    if (name.find("SE_FINISH") == 0) {
        bool expected = false;
        if (g_playedFinish.compare_exchange_strong(expected, true)) {
            if (g_audio) g_audio->StopCategory(0);
            if (fpCategorySetVolume) fpCategorySetVolume(0, g_originalBgmVolume);
            std::cout << "[Mod] Stopped custom BGM, restored cat 0 to " << g_originalBgmVolume << std::endl;
        }
    }

    if (name.find("BGM_") == 0) {
        if (name == "BGM_MONSTERTRUCK_01") {
            return 0;
        }

        auto isGpFinal = [&]() -> bool {
            const std::string p = "BGM_GP_";
            const std::string s = "_FINAL_1_2";
            if (name.size() <= p.size() + s.size()) return false;
            if (name.find(p) != 0) return false;
            if (name.rfind(s) != name.size() - s.size()) return false;
            for (size_t i = p.size(); i < name.size() - s.size(); i++)
                if (!isdigit((unsigned char)name[i])) return false;
            return true;
        };
        bool isCustomBgm = (name == "BGM_LAP1" || name == "BGM_LAP2_FORCE" || isGpFinal());

        if (!isCustomBgm && fpCategorySetVolume)
            fpCategorySetVolume(0, g_originalBgmVolume);

        if (g_audio) g_audio->StopCategory(0);

        if (isCustomBgm && !g_preloadedSounds.empty()) {
            g_playedFinish.store(false);
            if (fpCategoryGetVolume) g_originalBgmVolume = fpCategoryGetVolume(0);
            if (fpCategorySetVolume) fpCategorySetVolume(0, 0.0f);
            std::cout << "[Mod] Muted cat 0 (was " << g_originalBgmVolume << "), playing custom BGM." << std::endl;
            int idx;
            do { idx = std::uniform_int_distribution<int>(0, (int)g_preloadedSounds.size() - 1)(g_rng); }
            while (idx == g_lastPlayedIndex && g_preloadedSounds.size() > 1);
            g_lastPlayedIndex = idx;
            g_audio->PlayPreloaded(g_preloadedSounds[idx], g_customBgmVolume, 0, !g_playNewMusic);
        }
    }

    if (fpStartOriginal) return fpStartOriginal(player);
    return 0;
}

void InitHooks() {
    HMODULE hGame = GetModuleHandleA(nullptr);

    if (MH_Initialize() != MH_OK) { std::cout << "[Mod] MinHook Init failed.\n"; return; }

    const char* sigSetCueName = "48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC 20 45 33 F6 49 8B F0";
    uintptr_t addrSetCueName = PatternScan(hGame, sigSetCueName);
    if (addrSetCueName) {
        std::cout << "[Mod] Found criAtomExPlayer_SetCueName at: " << (void*)addrSetCueName << std::endl;
        if (MH_CreateHook((void*)addrSetCueName, &Hook_SetCueName, (void**)&fpSetCueNameOriginal) != MH_OK) {
            std::cout << "[Mod] CreateHook failed.\n"; return;
        }
        if (MH_EnableHook((void*)addrSetCueName) != MH_OK) {
            std::cout << "[Mod] EnableHook failed.\n"; return;
        }
        std::cout << "[Mod] SetCueName Hook enabled!\n";
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExPlayer_SetCueName.\n";
    }

    const char* sigSetCat = "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 50 48 8B D9 8B FA";
    uintptr_t addrSetCat = PatternScan(hGame, sigSetCat);
    if (addrSetCat) {
        std::cout << "[Mod] Found criAtomExPlayer_SetCategoryById at: " << (void*)addrSetCat << std::endl;
        MH_CreateHook((void*)addrSetCat, &Hook_SetCategoryById, (void**)&fpSetCategoryByIdOriginal);
        MH_EnableHook((void*)addrSetCat);
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExPlayer_SetCategoryById.\n";
    }

    const char* sigStart = "48 89 5C 24 ?? 57 48 83 EC 20 48 8B F9 48 85 C9 75 ?? 44 8D 41 ?? 48 8D 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C8 FF";
    uintptr_t addrStart = PatternScan(hGame, sigStart);
    if (addrStart) {
        std::cout << "[Mod] Found criAtomExPlayer_Start at: " << (void*)addrStart << std::endl;
        MH_CreateHook((void*)addrStart, &Hook_Start, (void**)&fpStartOriginal);
        MH_EnableHook((void*)addrStart);
        std::cout << "[Mod] Start Hook enabled!\n";
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExPlayer_Start.\n";
    }

    const char* sigLoadAcb = "48 89 5C 24 ?? 48 89 7C 24 ?? 55 48 8D 6C 24 ?? 48 81 EC";
    uintptr_t addrLoadAcb = PatternScan(hGame, sigLoadAcb);
    if (addrLoadAcb) {
        std::cout << "[Mod] Found criAtomExAcb_LoadAcbFile at: " << (void*)addrLoadAcb << std::endl;
        MH_CreateHook((void*)addrLoadAcb, &Hook_LoadAcbFile, (void**)&fpLoadAcbFileOriginal);
        MH_EnableHook((void*)addrLoadAcb);
    }

    const char* sigCatSetVol = "40 53 48 83 EC 30 48 0F BF D9";
    uintptr_t addrCatSetVol = PatternScan(hGame, sigCatSetVol);
    if (addrCatSetVol) {
        std::cout << "[Mod] Found criAtomExCategory_SetVolume at: " << (void*)addrCatSetVol << std::endl;
        fpCategorySetVolume = (criAtomExCategory_SetVolume_t)addrCatSetVol;
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExCategory_SetVolume.\n";
    }

    const char* sigCatVol = "40 53 48 83 EC 20 83 64 24 ?? 00";
    uintptr_t addrCatVol = PatternScan(hGame, sigCatVol);
    if (addrCatVol) {
        std::cout << "[Mod] Found criAtomExCategory_GetVolume at: " << (void*)addrCatVol << std::endl;
        fpCategoryGetVolume = (criAtomExCategory_GetVolume_t)addrCatVol;
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExCategory_GetVolume.\n";
    }
}

void InputLoop() {
    AudioEngine audio;
    if (!audio.Init()) {
        MessageBoxA(nullptr, "Failed to Init XAudio2", "Mod Error", MB_OK);
        return;
    }
    g_audio = &audio;
    audio.m_onBgmFinished = []() {
        if (g_playedFinish.load() || g_preloadedSounds.empty()) return;
        int idx;
        do { idx = std::uniform_int_distribution<int>(0, (int)g_preloadedSounds.size() - 1)(g_rng); }
        while (idx == g_lastPlayedIndex && g_preloadedSounds.size() > 1);
        g_lastPlayedIndex = idx;
        g_audio->PlayPreloaded(g_preloadedSounds[idx], g_customBgmVolume, 0, false);
        std::cout << "[Mod] PlayNewMusic: shuffled to next track." << std::endl;
    };

    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);

    char pathBuffer[MAX_PATH];
    if (GetModuleFileNameA(g_hModule, pathBuffer, MAX_PATH) == 0) {
        std::cout << "[Mod] Error getting DLL path." << std::endl;
        return;
    }

    std::string dllDir = pathBuffer;
    size_t lastSlash = dllDir.find_last_of("\\/");
    if (lastSlash != std::string::npos)
        dllDir = dllDir.substr(0, lastSlash);

    std::string settingsPath = dllDir + "\\settings.txt";
    std::ifstream settingsFile(settingsPath);
    if (settingsFile.is_open()) {
        std::string line;
        while (std::getline(settingsFile, line)) {
            if (line.find("PlayNewMusic:") != std::string::npos) {
                g_playNewMusic = (line.find("true") != std::string::npos);
                std::cout << "[Mod] PlayNewMusic: " << (g_playNewMusic ? "true" : "false") << std::endl;
            }
            if (line.find("MuteOnUnfocus:") != std::string::npos) {
                g_muteOnUnfocus = (line.find("true") != std::string::npos);
                std::cout << "[Mod] MuteOnUnfocus: " << (g_muteOnUnfocus ? "true" : "false") << std::endl;
            }
            if (line.find("Volume:") != std::string::npos) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string val = line.substr(colon + 1);
                    char* end;
                    float v = std::strtof(val.c_str(), &end);
                    if (end != val.c_str() && v >= 0.0f && v <= 5.0f)
                        g_customBgmVolume = v;
                }
            }
        }
    } else {
        std::cout << "[Mod] No settings.txt found, using defaults." << std::endl;
    }

    std::string musicDir = dllDir + "\\music";

    for (const char* ext : { "\\*.wav", "\\*.mp3", "\\*.ogg", "\\*.adx", "\\*.brstm", "\\*.flac" }) {
        WIN32_FIND_DATAA findData;
        HANDLE hFind = FindFirstFileA((musicDir + ext).c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::string fullPath = musicDir + "\\" + findData.cFileName;
                g_soundPaths.push_back(fullPath);
                std::cout << "[Mod] Found: " << fullPath << std::endl;
            } while (FindNextFileA(hFind, &findData));
            FindClose(hFind);
        }
    }
    if (g_soundPaths.empty())
        std::cout << "[Mod] No music files found in 'music' folder!" << std::endl;

    for (const auto& path : g_soundPaths) {
        WavData data;
        if (AudioLoader::Load(path, data)) {
            g_preloadedSounds.push_back(std::move(data));
        }
    }
    std::cout << "[Mod] Pre-loaded " << g_preloadedSounds.size() << " BGM sound(s)." << std::endl;

    std::cout << "[Mod] BGM Replacement loaded." << std::endl;

    InitHooks();

    DWORD ourPid = GetCurrentProcessId();
    bool wasUnfocused = false;

    while (bIsRunning) {
        audio.Update();

        if (g_muteOnUnfocus) {
            DWORD foregroundPid = 0;
            HWND fg = GetForegroundWindow();
            if (fg) GetWindowThreadProcessId(fg, &foregroundPid);
            bool unfocused = (foregroundPid != ourPid);
            if (unfocused != wasUnfocused) {
                wasUnfocused = unfocused;
                audio.SetCategoryVolume(0, unfocused ? 0.0f : g_customBgmVolume);
                std::cout << "[Mod] " << (unfocused ? "Unfocused" : "Focused") << " — volume: "
                          << (unfocused ? 0 : (int)(g_customBgmVolume * 100)) << "%" << std::endl;
            }
        }

        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
            static bool prevUp = false, prevDown = false;
            bool curUp = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
            bool curDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
            if (curUp && !prevUp) {
                g_customBgmVolume = std::min(5.0f, g_customBgmVolume + 0.1f);
                std::cout << "[Mod] Volume: " << (int)(g_customBgmVolume * 100) << "%" << std::endl;
                audio.SetCategoryVolume(0, g_customBgmVolume);
            }
            if (curDown && !prevDown) {
                g_customBgmVolume = std::max(0.0f, g_customBgmVolume - 0.1f);
                std::cout << "[Mod] Volume: " << (int)(g_customBgmVolume * 100) << "%" << std::endl;
                audio.SetCategoryVolume(0, g_customBgmVolume);
            }
            if ((curUp && !prevUp) || (curDown && !prevDown)) {
                std::ofstream out(dllDir + "\\settings.txt");
                if (out.is_open()) {
                    out << "//Play another music file after one is finished instead of looping? true or false" << std::endl;
                    out << "PlayNewMusic: " << (g_playNewMusic ? "true" : "false") << std::endl;
                    out << "//Mute custom BGM when game window loses focus" << std::endl;
                    out << "MuteOnUnfocus: " << (g_muteOnUnfocus ? "true" : "false") << std::endl;
                    out << "//Custom BGM volume (0.0 - 5.0)" << std::endl;
                    out << "Volume: " << g_customBgmVolume << std::endl;
                }
            }
            prevUp = curUp;
            prevDown = curDown;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    g_audio = nullptr;
}

extern "C" __declspec(dllexport) void start_mod() {
    bIsRunning = true;
    std::thread(InputLoop).detach();
}

extern "C" __declspec(dllexport) void uninstall_mod() {
    bIsRunning = false;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
        break;
    case DLL_PROCESS_DETACH:
        bIsRunning = false;
        break;
    }
    return TRUE;
}
