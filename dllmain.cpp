#include <windows.h>
#include <thread>
#include <atomic>
#include <iostream>
#include "AudioEngine.h"

// Link against User32.lib for MessageBox and Input
#pragma comment(lib, "user32.lib")

// Global atomic to control the thread
std::atomic<bool> bIsRunning{ false };
HMODULE g_hModule = nullptr;

// The background worker thread
void InputLoop() {
    AudioEngine audio;
    if (!audio.Init()) {
        MessageBoxA(nullptr, "Failed to Init XAudio2", "Mod Error", MB_OK);
        return;
    }

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

    std::string soundPath = dllDir + "\\test.wav";

    if (GetFileAttributesA(soundPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::cout << "[Mod] Sound file found at: " << soundPath << std::endl;
    } else {
        std::cout << "[Mod] ERROR: test.wav not found next to DLL! Expected at: " << soundPath << std::endl;
    }

    bool wasPressed = false;

    while (bIsRunning) {
        // Check for Numpad 1 or standard Number 1
        // GetAsyncKeyState checks hardware state asynchronously (bypassing UE input)
        bool isPressed = ((GetAsyncKeyState(VK_NUMPAD1) & 0x8000) != 0) || ((GetAsyncKeyState('1') & 0x8000) != 0);

        if (isPressed && !wasPressed) {
            // Key Down Event
            audio.PlayWave(soundPath);
        }

        wasPressed = isPressed;

        // Sleep to prevent high CPU usage in this thread
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
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
