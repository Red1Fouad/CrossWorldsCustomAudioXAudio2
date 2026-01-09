#include <windows.h>
#include <thread>
#include <atomic>
#include <iostream>
#include <cstdio>
#include <vector>
#include <sstream>
#include <psapi.h>
#include <map>
#include <mutex>
#include "AudioEngine.h"
#include "MinHook.h"

// Link against User32.lib for MessageBox and Input
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "libMinHook-x64-v141-md.lib")

// Global atomic to control the thread
std::atomic<bool> bIsRunning{ false };
HMODULE g_hModule = nullptr;

// Map to store Cue Names for Players
std::map<void*, std::string> g_playerCueNames;
std::map<void*, int> g_playerCategoryIds;
std::mutex g_playerMutex;

// Global Audio Engine access for the hooks
AudioEngine* g_audio = nullptr;
std::string g_soundPath;

// --- Pattern Scanning & Hooking Helpers ---

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

// --- Hooks ---

// Signature provided: 48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC 20 45 33 F6 49 8B F0
// The ending 'mov rsi, r8' implies R8 (3rd arg) is important. Likely SetCueName(player, acb, name).
typedef void (*criAtomExPlayer_SetCueName_t)(void* player, void* acb, const char* cueName);
criAtomExPlayer_SetCueName_t fpSetCueNameOriginal = nullptr;

// Helper to safely read the string without triggering C2712 (SEH + C++ Unwinding conflict)
bool SafeReadString(const char* src, char* dest, size_t maxLen) {
    __try {
        if (!src) return false;
        for (size_t i = 0; i < maxLen; ++i) {
            dest[i] = src[i];
            if (src[i] == '\0') return true;
        }
        dest[maxLen - 1] = '\0'; // Force null termination
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Hook_SetCueName(void* player, void* acb, const char* cueName) {
    char safeName[256];
    if (SafeReadString(cueName, safeName, sizeof(safeName))) {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        g_playerCueNames[player] = safeName;
        std::cout << "[Mod] CRI SetCueName: " << safeName << " (Player: " << player << ")" << std::endl;
    }

    // Call original
    if (fpSetCueNameOriginal) fpSetCueNameOriginal(player, acb, cueName);
}

// Hook SetCategoryById to track which category a player belongs to
typedef void (*criAtomExPlayer_SetCategoryById_t)(void* player, int id);
criAtomExPlayer_SetCategoryById_t fpSetCategoryByIdOriginal = nullptr;

void Hook_SetCategoryById(void* player, int id) {
    {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        g_playerCategoryIds[player] = id;
    }
    if (fpSetCategoryByIdOriginal) fpSetCategoryByIdOriginal(player, id);
}

// Hook Category SetVolume to see when game changes category volumes
typedef void (*criAtomExCategory_SetVolume_t)(int id, float vol);
criAtomExCategory_SetVolume_t fpCategorySetVolumeOriginal = nullptr;

void Hook_CategorySetVolume(int id, float vol) {
    std::cout << "[Mod] CRI Category SetVolume(" << id << ") -> " << vol << std::endl;
    if (fpCategorySetVolumeOriginal) fpCategorySetVolumeOriginal(id, vol);
}

// Hook Start: This is where the sound actually plays
typedef uint32_t (*criAtomExPlayer_Start_t)(void* player);
criAtomExPlayer_Start_t fpStartOriginal = nullptr;

// We need the GetVolume function pointer to call it manually
typedef float (*criAtomExCategory_GetVolume_t)(int id);
criAtomExCategory_GetVolume_t fpCategoryGetVolume = nullptr;

uint32_t Hook_Start(void* player) {
    std::string name = "Unknown";
    int catId = -1;
    {
        std::lock_guard<std::mutex> lock(g_playerMutex);
        if (g_playerCueNames.count(player)) {
            name = g_playerCueNames[player];
        }
        if (g_playerCategoryIds.count(player)) {
            catId = g_playerCategoryIds[player];
        }
    }
    
    std::stringstream ss;
    ss << "[Mod] CRI Start -> Cue: " << name;

    if (fpCategoryGetVolume) {
        if (catId != -1) ss << " | PlayerCat(" << catId << "): " << fpCategoryGetVolume(catId);
        
        // Scan first few categories to find Voice (usually 0-5)
        ss << " | AllCats: ";
        for (int i = 0; i < 6; ++i) ss << "[" << i << "]:" << fpCategoryGetVolume(i) << " ";
    }

    std::cout << ss.str() << std::endl;

    if (fpStartOriginal) return fpStartOriginal(player);
    return 0;
}

void InitHooks() {
    // Note: If the game uses a separate DLL for CriWare (e.g. cri_ware_pc.dll), 
    // change GetModuleHandleA(nullptr) to GetModuleHandleA("cri_ware_pc.dll").
    HMODULE hGame = GetModuleHandleA(nullptr);
    
    if (MH_Initialize() != MH_OK) { std::cout << "[Mod] MinHook Init failed.\n"; return; }

    // SetCueName
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

    // SetCategoryById
    const char* sigSetCat = "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC 50 48 8B D9 8B FA";
    uintptr_t addrSetCat = PatternScan(hGame, sigSetCat);
    if (addrSetCat) {
        std::cout << "[Mod] Found criAtomExPlayer_SetCategoryById at: " << (void*)addrSetCat << std::endl;
        MH_CreateHook((void*)addrSetCat, &Hook_SetCategoryById, (void**)&fpSetCategoryByIdOriginal);
        MH_EnableHook((void*)addrSetCat);
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExPlayer_SetCategoryById.\n";
    }

    // Category SetVolume
    const char* sigCatSetVol = "40 53 48 83 EC 30 48 0F BF D9";
    uintptr_t addrCatSetVol = PatternScan(hGame, sigCatSetVol);
    if (addrCatSetVol) {
        std::cout << "[Mod] Found criAtomExCategory_SetVolume at: " << (void*)addrCatSetVol << std::endl;
        MH_CreateHook((void*)addrCatSetVol, &Hook_CategorySetVolume, (void**)&fpCategorySetVolumeOriginal);
        MH_EnableHook((void*)addrCatSetVol);
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExCategory_SetVolume.\n";
    }

    // Start
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

    // Category GetVolume (Just get address, don't hook)
    const char* sigCatVol = "40 53 48 83 EC 20 83 64 24 ?? 00";
    uintptr_t addrCatVol = PatternScan(hGame, sigCatVol);
    if (addrCatVol) {
        std::cout << "[Mod] Found criAtomExCategory_GetVolume at: " << (void*)addrCatVol << std::endl;
        fpCategoryGetVolume = (criAtomExCategory_GetVolume_t)addrCatVol;
    } else {
        std::cout << "[Mod] Failed to find signature for criAtomExCategory_GetVolume.\n";
    }
}

// The background worker thread
void InputLoop() {
    // Initialize AudioEngine on this thread's stack, but expose it globally
    AudioEngine audio;
    if (!audio.Init()) {
        MessageBoxA(nullptr, "Failed to Init XAudio2", "Mod Error", MB_OK);
        return;
    }
    g_audio = &audio;

    // Create a console window to see the logs
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);

    // Get the directory of the current DLL so we can load the wav from next to it
    char pathBuffer[MAX_PATH];
    if (GetModuleFileNameA(g_hModule, pathBuffer, MAX_PATH) == 0) {
        std::cout << "[Mod] Error getting DLL path." << std::endl;
        return;
    }

    std::string dllDir = pathBuffer;
    size_t lastSlash = dllDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        dllDir = dllDir.substr(0, lastSlash);
    }

    g_soundPath = dllDir + "\\test.wav";

    if (GetFileAttributesA(g_soundPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::cout << "[Mod] Sound file found at: " << g_soundPath << std::endl;
    } else {
        std::cout << "[Mod] ERROR: test.wav not found next to DLL! Expected at: " << g_soundPath << std::endl;
    }

    // Initialize hooks AFTER setting up audio and paths
    InitHooks();

    bool wasPressed = false;

    while (bIsRunning) {
        // Check for Numpad 1 or standard Number 1
        // GetAsyncKeyState checks hardware state asynchronously (bypassing UE input)
        bool isPressed = ((GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0) || ((GetAsyncKeyState('1') & 0x8000) != 0);

        if (isPressed && !wasPressed) {
            // Key Down Event
            audio.PlayWave(g_soundPath);
        }

        wasPressed = isPressed;

        // Sleep to prevent high CPU usage in this thread
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Clear global pointer before exiting
    g_audio = nullptr;
}

extern "C" __declspec(dllexport) void start_mod() {
    bIsRunning = true;
    std::thread(InputLoop).detach();
}

extern "C" __declspec(dllexport) void uninstall_mod() {
    bIsRunning = false;
}

// Standard DLL Entry Point
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
