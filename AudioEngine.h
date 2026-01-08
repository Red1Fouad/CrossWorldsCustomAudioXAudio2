#pragma once
#include <xaudio2.h>
#include <string>
#include "WavLoader.h"

// Link libraries automatically (MSVC specific)
#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

class AudioEngine {
private:
    IXAudio2* pXAudio2 = nullptr;
    IXAudio2MasteringVoice* pMasterVoice = nullptr;

public:
    AudioEngine() {}

    ~AudioEngine() {
        if (pMasterVoice) pMasterVoice->DestroyVoice();
        if (pXAudio2) pXAudio2->Release();
        CoUninitialize();
    }

    bool Init() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return false;

        hr = XAudio2Create(&pXAudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) return false;

        hr = pXAudio2->CreateMasteringVoice(&pMasterVoice);
        if (FAILED(hr)) return false;

        return true;
    }

    void PlayWave(const std::string& path) {
        if (!pXAudio2) return;

        WavData data;
        if (!WavLoader::LoadWav(path, data)) {
            std::cout << "[Mod] Failed to load WAV file: " << path << "\n";
            return;
        }

        IXAudio2SourceVoice* pSourceVoice;
        HRESULT hr = pXAudio2->CreateSourceVoice(&pSourceVoice, &data.wfx);
        if (FAILED(hr)) return;

        XAUDIO2_BUFFER buffer = { 0 };
        buffer.AudioBytes = (UINT32)data.audioData.size();
        // Note: In a real engine, you must keep audioData alive until playback finishes.
        // For this simple test, we are leaking memory or risking a crash if the vector 
        // goes out of scope before playback ends. 
        // To fix this properly, we allocate a persistent buffer.
        BYTE* persistentBuffer = new BYTE[data.audioData.size()];
        memcpy(persistentBuffer, data.audioData.data(), data.audioData.size());
        
        buffer.pAudioData = persistentBuffer;
        buffer.Flags = XAUDIO2_END_OF_STREAM;

        hr = pSourceVoice->SubmitSourceBuffer(&buffer);
        if (FAILED(hr)) {
            pSourceVoice->DestroyVoice();
            delete[] persistentBuffer;
            return;
        }

        pSourceVoice->Start(0);
        
        // Cleanup logic: In a production mod, use callbacks (IXAudio2VoiceCallback) 
        // to delete persistentBuffer and DestroyVoice when OnStreamEnd fires.
    }
};
