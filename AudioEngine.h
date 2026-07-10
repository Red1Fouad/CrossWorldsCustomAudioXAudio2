#pragma once
#include <xaudio2.h>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <functional>
#include "WavLoader.h"

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

struct ActiveVoice {
    IXAudio2SourceVoice* pVoice;
    BYTE* pBuffer;
    int categoryId;
    bool isBgm;
    bool isFadingOut = false;
};

class AudioEngine {
private:
    IXAudio2* pXAudio2 = nullptr;
    IXAudio2MasteringVoice* pMasterVoice = nullptr;
    bool m_ownsDevice = false;
    std::vector<ActiveVoice> m_voices;
    std::mutex m_mutex;

public:
    std::function<void()> m_onBgmFinished;

    AudioEngine() {}

    ~AudioEngine() {
        StopAll();
        if (pMasterVoice) pMasterVoice->DestroyVoice();
        if (pXAudio2 && m_ownsDevice) pXAudio2->Release();
        if (m_ownsDevice) CoUninitialize();
    }

    bool Init() {
        extern IXAudio2* g_gameXAudio2;
        if (g_gameXAudio2) {
            pXAudio2 = g_gameXAudio2;
            m_ownsDevice = false;
            std::cout << "[Mod] Using game's XAudio2 instance." << std::endl;
            return true;
        }

        m_ownsDevice = true;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return false;

        hr = XAudio2Create(&pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) return false;

        hr = pXAudio2->CreateMasteringVoice(&pMasterVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, NULL, NULL, AudioCategory_GameEffects);
        if (FAILED(hr)) return false;

        return true;
    }

    void Update() {
        std::function<void()> bgmFinished;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_voices.begin(); it != m_voices.end(); ) {
                if (it->isFadingOut) {
                    float vol;
                    it->pVoice->GetVolume(&vol);
                    vol -= 0.02f;
                    if (vol <= 0.0f) {
                        it->pVoice->Stop();
                        it->pVoice->FlushSourceBuffers();
                        it->pVoice->DestroyVoice();
                        delete[] it->pBuffer;
                        it = m_voices.erase(it);
                        continue;
                    } else {
                        it->pVoice->SetVolume(vol);
                    }
                }

                XAUDIO2_VOICE_STATE state;
                it->pVoice->GetState(&state);
                if (state.BuffersQueued == 0) {
                    bool wasBgm = it->isBgm && !it->isFadingOut;
                    it->pVoice->DestroyVoice();
                    delete[] it->pBuffer;
                    it = m_voices.erase(it);
                    if (wasBgm && m_onBgmFinished)
                        bgmFinished = m_onBgmFinished;
                } else {
                    ++it;
                }
            }
        }
        if (bgmFinished) bgmFinished();
    }

    void StopAll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            v.pVoice->DestroyVoice();
            delete[] v.pBuffer;
        }
        m_voices.clear();
    }

    void SetCategoryVolume(int catId, float volume) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            if (v.categoryId == catId && !v.isFadingOut) {
                v.pVoice->SetVolume(volume);
            }
        }
    }

    void StopCategory(int catId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto& v : m_voices) {
            if (v.categoryId == catId) {
                v.isFadingOut = true;
            }
        }
    }

    void StopCategoryImmediate(int catId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_voices.begin(); it != m_voices.end(); ) {
            if (it->categoryId == catId) {
                it->pVoice->Stop();
                it->pVoice->FlushSourceBuffers();
                it->pVoice->DestroyVoice();
                delete[] it->pBuffer;
                it = m_voices.erase(it);
            } else {
                ++it;
            }
        }
    }

    void PlayWave(const std::string& path, float volume, int categoryId) {
        if (!pXAudio2) return;

        WavData data;
        if (!AudioLoader::Load(path, data)) {
            std::cout << "[Mod] Failed to load audio file: " << path << "\n";
            return;
        }

        PlayPreloaded(data, volume, categoryId);
    }

    void PlayPreloaded(const WavData& data, float volume, int categoryId, bool allowLoop = true) {
        if (!pXAudio2) return;

        bool isBgm = (categoryId == 0);

        if (isBgm) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& v : m_voices) {
                if (v.isBgm) {
                    v.isFadingOut = true;
                }
            }
        }

        IXAudio2SourceVoice* pSourceVoice;
        HRESULT hr = pXAudio2->CreateSourceVoice(&pSourceVoice, &data.wfx);
        if (FAILED(hr)) return;

        XAUDIO2_BUFFER buffer = { 0 };
        buffer.AudioBytes = (UINT32)data.audioData.size();
        BYTE* persistentBuffer = new BYTE[data.audioData.size()];
        memcpy(persistentBuffer, data.audioData.data(), data.audioData.size());

        buffer.pAudioData = persistentBuffer;
        buffer.Flags = XAUDIO2_END_OF_STREAM;
        if (data.hasLoop && data.loopLength > 0) {
            buffer.LoopBegin = data.loopBegin;
            buffer.LoopLength = data.loopLength;
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        } else if (isBgm && allowLoop) {
            buffer.LoopCount = XAUDIO2_LOOP_INFINITE;
        }

        hr = pSourceVoice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            pSourceVoice->DestroyVoice();
            delete[] persistentBuffer;
            return;
        }

        hr = pSourceVoice->SetVolume(volume);
        if (FAILED(hr)) {
            std::cout << "[Mod] Warning: SetVolume failed (hr=" << std::hex << hr << ")\n";
        }
        pSourceVoice->Start(0);

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_voices.push_back({ pSourceVoice, persistentBuffer, categoryId, isBgm, false });
        }
    }
};
