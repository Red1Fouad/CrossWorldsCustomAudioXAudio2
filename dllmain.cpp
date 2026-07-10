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
std::vector<CompressedAudio> g_preloadedSounds;
std::vector<CompressedAudio> g_preloadedLobbySounds;
std::vector<CompressedAudio> g_preloadedTitleSounds;
std::atomic<bool> g_playedFinish{ false };
std::atomic<bool> g_bgmActive{ false };
bool g_playNewMusic = false;
bool g_muteOnUnfocus = false;
int g_activePool = 0; // 0=bgm, 1=lobby, 2=title
float g_customBgmVolume = 1.0f;
float g_originalBgmVolume = 1.0f;
std::vector<int> g_bgmOrder;
int g_bgmOrderPos = 0;
std::vector<int> g_lobbyOrder;
int g_lobbyOrderPos = 0;
std::vector<int> g_titleOrder;
int g_titleOrderPos = 0;
std::mt19937 g_rng(std::random_device{}());

void PlayCompressed(CompressedAudio& audio, float volume, int categoryId, bool allowLoop = true) {
    size_t bs = audio.filename.find_last_of("\\/");
    std::string fname = (bs != std::string::npos) ? audio.filename.substr(bs + 1) : audio.filename;
    std::cout << "[Mod] Playing: " << fname << std::endl;
    WavData data;
    if (!AudioLoader::DecodeToPcm(audio, data)) return;
    AudioLoader::NormalizePcm16(data);
    AudioLoader::ScalePcm16(data, audio.volumeMul);
    g_audio->PlayPreloaded(data, volume, categoryId, allowLoop);
}

static std::string WideToUtf8(const wchar_t* wide) {
    if (!wide || !*wide) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string utf8(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, &utf8[0], len, nullptr, nullptr);
    utf8.resize(len - 1);
    return utf8;
}

static std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
    wide.resize(len - 1);
    return wide;
}

struct TrackMeta {
    float weight = 1.0f;
    float volumeMul = 1.0f;
};

std::map<std::string, TrackMeta> ParseTrackMeta(const std::string& dir) {
    std::map<std::string, TrackMeta> meta;
    std::wstring wdir = Utf8ToWide(dir);
    std::ifstream f(wdir + L"\\tracks.txt");
    if (!f.is_open()) return meta;
    std::string line;
    while (std::getline(f, line)) {
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos) continue;
        line = line.substr(s);
        if (line.empty() || line[0] == ';' || line[0] == '/') continue;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        size_t ne = name.find_last_not_of(" \t");
        if (ne != std::string::npos) name = name.substr(0, ne + 1);
        std::string rest = line.substr(colon + 1);
        for (auto& c : rest) if (c == ',') c = ' ';
        std::stringstream ss(rest);
        TrackMeta tm;
        float v;
        int idx = 0;
        while (ss >> v) {
            if (idx == 0) tm.weight = std::max(0.0f, v);
            else if (idx == 1) tm.volumeMul = std::max(0.0f, v);
            idx++;
        }
        meta[name] = tm;
    }
    return meta;
}

void BuildShuffleOrder(std::vector<int>& order, const std::vector<CompressedAudio>& pool) {
    order.clear();
    if (pool.empty()) return;
    std::vector<int> remaining;
    for (int i = 0; i < (int)pool.size(); i++)
        remaining.push_back(i);
    while (!remaining.empty()) {
        float total = 0;
        for (int idx : remaining)
            total += std::max(0.0f, pool[idx].weight);
        int selected = -1;
        if (total > 0) {
            std::uniform_real_distribution<float> dist(0.0f, total);
            float r = dist(g_rng);
            float accum = 0;
            for (int i = 0; i < (int)remaining.size(); i++) {
                accum += std::max(0.0f, pool[remaining[i]].weight);
                if (r < accum) { selected = remaining[i]; break; }
            }
        } else {
            selected = remaining[std::uniform_int_distribution<int>(0, (int)remaining.size() - 1)(g_rng)];
        }
        if (selected >= 0) {
            order.push_back(selected);
            remaining.erase(std::remove(remaining.begin(), remaining.end(), selected), remaining.end());
        }
    }
}

int GetNextShuffledIndex(std::vector<CompressedAudio>& pool, std::vector<int>& order, int& pos) {
    if (pool.empty()) return -1;
    if (pos >= (int)order.size()) {
        BuildShuffleOrder(order, pool);
        pos = 0;
    }
    if (order.empty()) return -1;
    return order[pos++];
}

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

    if (name.find("SE_FINISH") == 0) {
        bool expected = false;
        if (g_playedFinish.compare_exchange_strong(expected, true)) {
            if (g_audio) g_audio->StopCategory(0);
            if (fpCategorySetVolume) fpCategorySetVolume(0, g_originalBgmVolume);
            g_activePool = 0;
            g_bgmActive.store(false);
            std::cout << "[Mod] Stopped custom BGM, restored cat 0 to " << g_originalBgmVolume << std::endl;
        }
    }

    if (name.find("BGM_") == 0) {
        if (name == "BGM_MONSTERTRUCK_01" || name == "BGM_TOP_MENU" ||
            name.find("BGM_CHARASELECT_") == 0 || name.find("BGM_GARAGE_") == 0 ||
            name.find("BGM_PARTYRACE_") == 0 || name.find("BGM_PREVIEW_") == 0) {
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
        int poolId = 0; // 0=bgm, 1=lobby, 2=title
        if (name.find("BGM_LOBBY_") == 0) poolId = 1;
        else if (name.find("BGM_TITLE_") == 0) poolId = 2;

        bool isCustomBgm = (name == "BGM_LAP1" || name == "BGM_LAP2_FORCE" || isGpFinal() || poolId != 0);

        if (!isCustomBgm) {
            if (fpCategorySetVolume) fpCategorySetVolume(0, g_originalBgmVolume);
            if (g_audio) g_audio->StopCategory(0);
            if (fpStartOriginal) return fpStartOriginal(player);
            return 0;
        }

        auto getPool = [&](int pid) -> std::vector<CompressedAudio>& {
            if (pid == 1) return g_preloadedLobbySounds;
            if (pid == 2) return g_preloadedTitleSounds;
            return g_preloadedSounds;
        };
        auto getOrder = [&](int pid) -> std::vector<int>& {
            if (pid == 1) return g_lobbyOrder;
            if (pid == 2) return g_titleOrder;
            return g_bgmOrder;
        };
        auto getPos = [&](int pid) -> int& {
            if (pid == 1) return g_lobbyOrderPos;
            if (pid == 2) return g_titleOrderPos;
            return g_bgmOrderPos;
        };
        auto& pool = getPool(poolId);
        auto& order = getOrder(poolId);
        auto& pos = getPos(poolId);
        const char* poolName = poolId == 1 ? "lobby" : poolId == 2 ? "title" : "BGM";

        if (!pool.empty()) {
            if (poolId == g_activePool && g_bgmActive.load()) {
                return fpStartOriginal ? fpStartOriginal(player) : 0;
            }
            if (g_audio) g_audio->StopCategory(0);
            g_activePool = poolId;
            g_playedFinish.store(false);
            float curVol = fpCategoryGetVolume ? fpCategoryGetVolume(0) : 1.0f;
            if (curVol > 0.0f) g_originalBgmVolume = curVol;
            if (fpCategorySetVolume) fpCategorySetVolume(0, 0.0f);
            std::cout << "[Mod] Muted cat 0 (was " << g_originalBgmVolume << "), playing " << poolName << " music." << std::endl;
            int idx = GetNextShuffledIndex(pool, order, pos);
            PlayCompressed(pool[idx], g_customBgmVolume, 0, !g_playNewMusic);
            g_bgmActive.store(true);
        }
    }

    if (fpStartOriginal) return fpStartOriginal(player);
    return 0;
}

void InitHooks() {
    HMODULE hGame = GetModuleHandleA(nullptr);

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
        if (g_playedFinish.load()) return;
        g_bgmActive.store(false);
        auto getPool = [](int pid) -> std::vector<CompressedAudio>& {
            if (pid == 1) return g_preloadedLobbySounds;
            if (pid == 2) return g_preloadedTitleSounds;
            return g_preloadedSounds;
        };
        auto getOrder = [](int pid) -> std::vector<int>& {
            if (pid == 1) return g_lobbyOrder;
            if (pid == 2) return g_titleOrder;
            return g_bgmOrder;
        };
        auto getPos = [](int pid) -> int& {
            if (pid == 1) return g_lobbyOrderPos;
            if (pid == 2) return g_titleOrderPos;
            return g_bgmOrderPos;
        };
        const char* poolName = g_activePool == 1 ? "lobby" : g_activePool == 2 ? "title" : "BGM";
        auto& pool = getPool(g_activePool);
        auto& order = getOrder(g_activePool);
        auto& pos = getPos(g_activePool);
        if (pool.empty()) return;
        int idx = GetNextShuffledIndex(pool, order, pos);
        PlayCompressed(pool[idx], g_customBgmVolume, 0, false);
        g_bgmActive.store(true);
        std::cout << "[Mod] PlayNewMusic: shuffled to next " << poolName << " track." << std::endl;
    };

    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);

    wchar_t wPathBuffer[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, wPathBuffer, MAX_PATH) == 0) {
        std::cout << "[Mod] Error getting DLL path." << std::endl;
        return;
    }

    std::string dllDir = WideToUtf8(wPathBuffer);
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
    std::wstring wMusicDir = Utf8ToWide(musicDir);
    auto bgmMeta = ParseTrackMeta(musicDir);

    for (const wchar_t* ext : { L"\\*.wav", L"\\*.mp3", L"\\*.ogg", L"\\*.adx", L"\\*.brstm", L"\\*.flac", L"\\*.aac", L"\\*.m4a", L"\\*.aax" }) {
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW((wMusicDir + ext).c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) continue;
                std::string utf8Name = WideToUtf8(findData.cFileName);
                std::string fullPath = musicDir + "\\" + utf8Name;
                g_soundPaths.push_back(fullPath);
                std::cout << "[Mod] Found: " << fullPath << std::endl;
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
    }
    if (g_soundPaths.empty())
        std::cout << "[Mod] No music files found in 'music' folder!" << std::endl;

    for (const auto& path : g_soundPaths) {
        CompressedAudio ca;
        if (AudioLoader::LoadCompressed(path, ca)) {
            std::string fname = path;
            size_t bs = fname.find_last_of("\\/");
            if (bs != std::string::npos) fname = fname.substr(bs + 1);
            auto it = bgmMeta.find(fname);
            TrackMeta tm;
            if (it != bgmMeta.end()) tm = it->second;
            ca.weight = tm.weight;
            ca.volumeMul = tm.volumeMul;
            g_preloadedSounds.push_back(std::move(ca));
        }
    }
    std::cout << "[Mod] Pre-loaded " << g_preloadedSounds.size() << " BGM sound(s)." << std::endl;

    std::string lobbyDir = dllDir + "\\music_lobby";
    std::wstring wLobbyDir = Utf8ToWide(lobbyDir);
    auto lobbyMeta = ParseTrackMeta(lobbyDir);
    for (const wchar_t* ext : { L"\\*.wav", L"\\*.mp3", L"\\*.ogg", L"\\*.adx", L"\\*.brstm", L"\\*.flac", L"\\*.aac", L"\\*.m4a", L"\\*.aax" }) {
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW((wLobbyDir + ext).c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) continue;
                std::string utf8Name = WideToUtf8(findData.cFileName);
                std::string fullPath = lobbyDir + "\\" + utf8Name;
                CompressedAudio ca;
                if (AudioLoader::LoadCompressed(fullPath, ca)) {
                    auto it = lobbyMeta.find(utf8Name);
                    TrackMeta tm;
                    if (it != lobbyMeta.end()) tm = it->second;
                    ca.weight = tm.weight;
                    ca.volumeMul = tm.volumeMul;
                    g_preloadedLobbySounds.push_back(std::move(ca));
                }
                std::cout << "[Mod] Lobby: " << fullPath << std::endl;
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
    }
    std::cout << "[Mod] Pre-loaded " << g_preloadedLobbySounds.size() << " lobby sound(s)." << std::endl;

    std::string titleDir = dllDir + "\\music_title";
    std::wstring wTitleDir = Utf8ToWide(titleDir);
    auto titleMeta = ParseTrackMeta(titleDir);
    for (const wchar_t* ext : { L"\\*.wav", L"\\*.mp3", L"\\*.ogg", L"\\*.adx", L"\\*.brstm", L"\\*.flac", L"\\*.aac", L"\\*.m4a", L"\\*.aax" }) {
        WIN32_FIND_DATAW findData;
        HANDLE hFind = FindFirstFileW((wTitleDir + ext).c_str(), &findData);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(findData.cFileName, L".") == 0 || wcscmp(findData.cFileName, L"..") == 0) continue;
                std::string utf8Name = WideToUtf8(findData.cFileName);
                std::string fullPath = titleDir + "\\" + utf8Name;
                CompressedAudio ca;
                if (AudioLoader::LoadCompressed(fullPath, ca)) {
                    auto it = titleMeta.find(utf8Name);
                    TrackMeta tm;
                    if (it != titleMeta.end()) tm = it->second;
                    ca.weight = tm.weight;
                    ca.volumeMul = tm.volumeMul;
                    g_preloadedTitleSounds.push_back(std::move(ca));
                }
                std::cout << "[Mod] Title: " << fullPath << std::endl;
            } while (FindNextFileW(hFind, &findData));
            FindClose(hFind);
        }
    }
    std::cout << "[Mod] Pre-loaded " << g_preloadedTitleSounds.size() << " title sound(s)." << std::endl;

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
            static bool prevLeft = false, prevRight = false;
            bool curUp = (GetAsyncKeyState(VK_UP) & 0x8000) != 0;
            bool curDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
            bool curLeft = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
            bool curRight = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
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
            if (curRight && !prevRight) {
                auto getPool = [](int pid) -> std::vector<CompressedAudio>& {
                    if (pid == 1) return g_preloadedLobbySounds;
                    if (pid == 2) return g_preloadedTitleSounds;
                    return g_preloadedSounds;
                };
                auto getOrder = [](int pid) -> std::vector<int>& {
                    if (pid == 1) return g_lobbyOrder;
                    if (pid == 2) return g_titleOrder;
                    return g_bgmOrder;
                };
                auto getPos = [](int pid) -> int& {
                    if (pid == 1) return g_lobbyOrderPos;
                    if (pid == 2) return g_titleOrderPos;
                    return g_bgmOrderPos;
                };
                const char* poolName = g_activePool == 1 ? "lobby" : g_activePool == 2 ? "title" : "BGM";
                auto& pool = getPool(g_activePool);
                auto& order = getOrder(g_activePool);
                auto& pos = getPos(g_activePool);
                if (!pool.empty()) {
                    int idx = GetNextShuffledIndex(pool, order, pos);
                    audio.StopCategoryImmediate(0);
                    PlayCompressed(pool[idx], g_customBgmVolume, 0, !g_playNewMusic);
                    g_bgmActive.store(true);
                    std::cout << "[Mod] Skip -> " << poolName << " track." << std::endl;
                }
            }
            if (curLeft && !prevLeft) {
                auto getPool = [](int pid) -> std::vector<CompressedAudio>& {
                    if (pid == 1) return g_preloadedLobbySounds;
                    if (pid == 2) return g_preloadedTitleSounds;
                    return g_preloadedSounds;
                };
                auto getOrder = [](int pid) -> std::vector<int>& {
                    if (pid == 1) return g_lobbyOrder;
                    if (pid == 2) return g_titleOrder;
                    return g_bgmOrder;
                };
                auto getPos = [](int pid) -> int& {
                    if (pid == 1) return g_lobbyOrderPos;
                    if (pid == 2) return g_titleOrderPos;
                    return g_bgmOrderPos;
                };
                const char* poolName = g_activePool == 1 ? "lobby" : g_activePool == 2 ? "title" : "BGM";
                auto& pool = getPool(g_activePool);
                auto& order = getOrder(g_activePool);
                auto& pos = getPos(g_activePool);
                if (!pool.empty() && pos > 0) {
                    int curIdx = order[pos - 1];
                    audio.StopCategoryImmediate(0);
                    PlayCompressed(pool[curIdx], g_customBgmVolume, 0, !g_playNewMusic);
                    g_bgmActive.store(true);
                    std::cout << "[Mod] Restart current " << poolName << " track." << std::endl;
                }
            }
            prevUp = curUp;
            prevDown = curDown;
            prevLeft = curLeft;
            prevRight = curRight;
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
        if (MH_Initialize() == MH_OK) {
            HMODULE hGame = GetModuleHandleA(nullptr);
            const char* sigSetCueName = "48 89 5C 24 ?? 48 89 6C 24 ?? 56 57 41 56 48 83 EC 20 45 33 F6 49 8B F0";
            uintptr_t addrSetCueName = PatternScan(hGame, sigSetCueName);
            if (addrSetCueName) {
                if (MH_CreateHook((void*)addrSetCueName, &Hook_SetCueName, (void**)&fpSetCueNameOriginal) == MH_OK)
                    MH_EnableHook((void*)addrSetCueName);
            }
        }
        break;
    case DLL_PROCESS_DETACH:
        bIsRunning = false;
        break;
    }
    return TRUE;
}
