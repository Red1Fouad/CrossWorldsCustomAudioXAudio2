#pragma once
#include <windows.h>
#include <fstream>
#include <vector>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <string>

#if __has_include("dr_mp3.h")
#include "dr_mp3.h"
#define ENABLE_MP3
#endif

#if __has_include("stb_vorbis.c")
#include "stb_vorbis.c"
#define ENABLE_OGG
#endif

#if __has_include("dr_flac.h")
#include "dr_flac.h"
#define ENABLE_FLAC
#endif

#if __has_include("aacdec.h")
#include "aacdec.h"
#define ENABLE_AAC
#endif

struct WavData {
    WAVEFORMATEX wfx;
    std::vector<BYTE> audioData;
    bool hasLoop = false;
    UINT32 loopBegin = 0;
    UINT32 loopLength = 0;
};

struct TrackInfo {
    WavData data;
    float weight = 1.0f;
};

class AudioLoader {
public:
    static bool Load(const std::string& filename, WavData& outData) {
        std::string ext = "";
        size_t dot = filename.find_last_of(".");
        if (dot != std::string::npos) {
            ext = filename.substr(dot);
            for (auto& c : ext) c = tolower(c);
        }

        if (ext == ".mp3") {
#ifdef ENABLE_MP3
            return LoadMp3(filename, outData);
#else
            std::cout << "[AudioLoader] Error: MP3 support not compiled (dr_mp3.h missing)." << std::endl;
            return false;
#endif
        }
        else if (ext == ".ogg") {
#ifdef ENABLE_OGG
            return LoadOgg(filename, outData);
#else
            std::cout << "[AudioLoader] Error: OGG support not compiled (stb_vorbis.c missing)." << std::endl;
            return false;
#endif
        }
        else if (ext == ".adx") {
            return LoadAdx(filename, outData);
        }
        else if (ext == ".brstm") {
            return LoadBrstm(filename, outData);
        }
        else if (ext == ".flac") {
#ifdef ENABLE_FLAC
            return LoadFlac(filename, outData);
#else
            std::cout << "[AudioLoader] Error: FLAC support not compiled (dr_flac.h missing)." << std::endl;
            return false;
#endif
        }
        else if (ext == ".m4a" || ext == ".aac") {
#ifdef ENABLE_AAC
            return LoadAac(filename, outData);
#else
            std::cout << "[AudioLoader] Error: AAC support not compiled (Helix AAC missing)." << std::endl;
            return false;
#endif
        }
        else {
            return LoadWav(filename, outData);
        }
    }

private:
    static std::wstring Utf8ToWide(const std::string& utf8) {
        if (utf8.empty()) return {};
        int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (len <= 0) return {};
        std::wstring wide(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
        wide.resize(len - 1);
        return wide;
    }

    static bool ReadFileIntoMemory(const std::string& utf8Path, std::vector<uint8_t>& outData) {
        std::wstring wpath = Utf8ToWide(utf8Path);
        FILE* f = _wfopen(wpath.c_str(), L"rb");
        if (!f) {
            std::cout << "[AudioLoader] Error: Could not open file: " << utf8Path << std::endl;
            return false;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0) { fclose(f); return false; }
        outData.resize(sz);
        if (fread(outData.data(), 1, sz, f) != (size_t)sz) {
            fclose(f);
            return false;
        }
        fclose(f);
        return true;
    }

    static bool ReadWavFromMemory(const std::vector<uint8_t>& data, WavData& outData) {
        size_t pos = 0;
        if (pos + 4 > data.size() || memcmp(data.data(), "RIFF", 4) != 0) {
            std::cout << "[WavLoader] Error: Invalid RIFF header." << std::endl;
            return false;
        }
        pos += 8;
        if (pos + 4 > data.size() || memcmp(data.data() + pos, "WAVE", 4) != 0) {
            std::cout << "[WavLoader] Error: Invalid WAVE header." << std::endl;
            return false;
        }
        pos += 4;

        bool foundFmt = false;
        bool foundData = false;

        while (pos + 8 <= data.size() && (!foundFmt || !foundData)) {
            char subChunkId[4];
            memcpy(subChunkId, data.data() + pos, 4);
            pos += 4;
            uint32_t subChunkSize;
            memcpy(&subChunkSize, data.data() + pos, 4);
            pos += 4;

            if (memcmp(subChunkId, "fmt ", 4) == 0) {
                if (subChunkSize >= 18 && pos + 18 <= data.size()) {
                    memcpy(&outData.wfx, data.data() + pos, 18);
                    pos += subChunkSize;
                } else if (pos + 16 <= data.size()) {
                    memcpy(&outData.wfx, data.data() + pos, 16);
                    outData.wfx.cbSize = 0;
                    pos += subChunkSize;
                } else {
                    break;
                }
                foundFmt = true;
            }
            else if (memcmp(subChunkId, "data", 4) == 0) {
                if (pos + subChunkSize <= data.size()) {
                    outData.audioData.resize(subChunkSize);
                    memcpy(outData.audioData.data(), data.data() + pos, subChunkSize);
                    pos += subChunkSize;
                } else {
                    break;
                }
                foundData = true;
            }
            else {
                pos += subChunkSize;
            }

            if (subChunkSize % 2 != 0) pos++;
        }

        if (!foundFmt) std::cout << "[WavLoader] Error: 'fmt ' chunk not found." << std::endl;
        if (!foundData) std::cout << "[WavLoader] Error: 'data' chunk not found." << std::endl;

        return foundFmt && foundData;
    }

    static bool LoadWav(const std::string& filename, WavData& outData) {
        std::vector<uint8_t> fileData;
        if (!ReadFileIntoMemory(filename, fileData)) return false;
        return ReadWavFromMemory(fileData, outData);
    }

    static bool LoadMp3(const std::string& filename, WavData& outData) {
#ifdef ENABLE_MP3
        std::vector<uint8_t> fileData;
        if (!ReadFileIntoMemory(filename, fileData)) return false;

        drmp3_config config;
        drmp3_uint64 totalPCMFrameCount;

        drmp3_int16* pSampleData = drmp3_open_memory_and_read_pcm_frames_s16(fileData.data(), fileData.size(), &config, &totalPCMFrameCount, NULL);

        if (!pSampleData) {
            std::cout << "[AudioLoader] Error: Failed to load MP3: " << filename << std::endl;
            return false;
        }

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)config.channels;
        outData.wfx.nSamplesPerSec = config.sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = outData.wfx.nChannels * (outData.wfx.wBitsPerSample / 8);
        outData.wfx.nAvgBytesPerSec = outData.wfx.nSamplesPerSec * outData.wfx.nBlockAlign;
        outData.wfx.cbSize = 0;

        size_t dataSize = (size_t)totalPCMFrameCount * config.channels * sizeof(drmp3_int16);
        outData.audioData.resize(dataSize);
        memcpy(outData.audioData.data(), pSampleData, dataSize);

        drmp3_free(pSampleData, NULL);
        return true;
#else
        return false;
#endif
    }

    static bool LoadOgg(const std::string& filename, WavData& outData) {
#ifdef ENABLE_OGG
        std::vector<uint8_t> fileData;
        if (!ReadFileIntoMemory(filename, fileData)) return false;

        int error = 0;
        stb_vorbis* v = stb_vorbis_open_memory(fileData.data(), (int)fileData.size(), &error, NULL);
        if (!v) {
            std::cout << "[AudioLoader] Error: Failed to load OGG: " << filename << std::endl;
            return false;
        }

        stb_vorbis_info info = stb_vorbis_get_info(v);
        int channels = info.channels;
        int sampleRate = info.sample_rate;
        int totalSamples = stb_vorbis_stream_length_in_samples(v);

        if (totalSamples <= 0) {
            stb_vorbis_close(v);
            return false;
        }

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)channels;
        outData.wfx.nSamplesPerSec = sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = outData.wfx.nChannels * 2;
        outData.wfx.nAvgBytesPerSec = outData.wfx.nSamplesPerSec * outData.wfx.nBlockAlign;
        outData.wfx.cbSize = 0;

        size_t dataSize = totalSamples * channels * sizeof(short);
        outData.audioData.resize(dataSize);
        int samplesDecoded = stb_vorbis_get_samples_short_interleaved(v, channels, (short*)outData.audioData.data(), (int)(dataSize / sizeof(short)));

        stb_vorbis_close(v);

        if (samplesDecoded <= 0) {
            std::cout << "[AudioLoader] Error: Failed to decode OGG: " << filename << std::endl;
            return false;
        }

        outData.audioData.resize(samplesDecoded * channels * sizeof(short));
        return true;
#else
        return false;
#endif
    }

    static bool LoadFlac(const std::string& filename, WavData& outData) {
#ifdef ENABLE_FLAC
        std::vector<uint8_t> fileData;
        if (!ReadFileIntoMemory(filename, fileData)) return false;

        unsigned int channels;
        unsigned int sampleRate;
        drflac_uint64 totalPCMFrameCount;

        drflac_int16* pSampleData = drflac_open_memory_and_read_pcm_frames_s16(fileData.data(), fileData.size(), &channels, &sampleRate, &totalPCMFrameCount, NULL);

        if (!pSampleData) {
            std::cout << "[AudioLoader] Error: Failed to load FLAC: " << filename << std::endl;
            return false;
        }

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)channels;
        outData.wfx.nSamplesPerSec = sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = outData.wfx.nChannels * (outData.wfx.wBitsPerSample / 8);
        outData.wfx.nAvgBytesPerSec = outData.wfx.nSamplesPerSec * outData.wfx.nBlockAlign;
        outData.wfx.cbSize = 0;

        size_t dataSize = (size_t)totalPCMFrameCount * channels * sizeof(drflac_int16);
        outData.audioData.resize(dataSize);
        memcpy(outData.audioData.data(), pSampleData, dataSize);

        drflac_free(pSampleData, NULL);
        return true;
#else
        return false;
#endif
    }

    static bool LoadAdx(const std::string& filename, WavData& outData) {
        std::vector<uint8_t> fileData;
        if (!ReadFileIntoMemory(filename, fileData)) return false;

        if (fileData.size() < 20) return false;

        uint16_t offset = ((uint16_t)fileData[2] << 8) | fileData[3];
        uint8_t channelCount = fileData[7];
        uint32_t sampleRate = ((uint32_t)fileData[8] << 24) | ((uint32_t)fileData[9] << 16) |
                              ((uint32_t)fileData[10] << 8) | fileData[11];
        uint32_t totalSamples = ((uint32_t)fileData[12] << 24) | ((uint32_t)fileData[13] << 16) |
                                ((uint32_t)fileData[14] << 8) | fileData[15];
        uint16_t highpassFreq = ((uint16_t)fileData[16] << 8) | fileData[17];
        uint8_t version = fileData[18];

        bool hasLoop = false;
        uint32_t loopByteStart = 0;
        uint32_t loopByteEnd = 0;
        if (version >= 3) {
            uint32_t loopOffset = (version == 4) ? 0x24 : 0x18;
            if (loopOffset + 20 <= fileData.size()) {
                uint32_t loopFlag = ((uint32_t)fileData[loopOffset] << 24) | ((uint32_t)fileData[loopOffset+1] << 16) |
                                    ((uint32_t)fileData[loopOffset+2] << 8) | fileData[loopOffset+3];
                hasLoop = loopFlag != 0;
                loopByteStart = ((uint32_t)fileData[loopOffset+4] << 24) | ((uint32_t)fileData[loopOffset+5] << 16) |
                                ((uint32_t)fileData[loopOffset+6] << 8) | fileData[loopOffset+7];
                loopByteEnd = ((uint32_t)fileData[loopOffset+12] << 24) | ((uint32_t)fileData[loopOffset+13] << 16) |
                              ((uint32_t)fileData[loopOffset+14] << 8) | fileData[loopOffset+15];
            }
        }

        uint32_t audioOffset = offset + 4;
        if (audioOffset >= fileData.size()) return false;

        const uint8_t* encoded = fileData.data() + audioOffset;
        size_t encodedSize = fileData.size() - audioOffset;

        double factor = (double)highpassFreq / (double)sampleRate;
        double a = 1.4142135623730951 - cos(6.283185307179586 * factor);
        double b = 1.4142135623730951 - 1.0;
        double c = (a - sqrt((a + b) * (a - b))) / b;
        double coeff1 = 2.0 * c;
        double coeff2 = -(c * c);

        struct AdxDec {
            double c1, c2;
            int32_t prev1 = 0, prev2 = 0;
            int32_t Decode(int32_t sample, int32_t scale) {
                int32_t pred = (int32_t)(c1 * prev1 + c2 * prev2);
                int32_t result = sample * scale + pred;
                if (result > 32767) result = 32767;
                if (result < -32768) result = -32768;
                prev2 = prev1;
                prev1 = result;
                return result;
            }
        };

        std::vector<AdxDec> decoders(channelCount, AdxDec{coeff1, coeff2, 0, 0});
        std::vector<std::vector<int16_t>> channelBuf(channelCount);
        for (auto& buf : channelBuf) buf.reserve(totalSamples);

        size_t pos = 0;
        while (pos + 18 <= encodedSize) {
            for (int ch = 0; ch < channelCount && pos + 18 <= encodedSize; ch++) {
                int32_t scale = ((int32_t)encoded[pos] << 8) | encoded[pos + 1];
                auto& dec = decoders[ch];

                for (int i = 0; i < 16; i++) {
                    uint8_t b = encoded[pos + 2 + i];
                    int32_t hi = (b >> 4) & 0xF;
                    int32_t lo = b & 0xF;
                    if (hi & 8) hi -= 16;
                    if (lo & 8) lo -= 16;
                    channelBuf[ch].push_back((int16_t)dec.Decode(hi, scale));
                    channelBuf[ch].push_back((int16_t)dec.Decode(lo, scale));
                }
                pos += 18;
            }
        }

        if (channelBuf[0].empty()) return false;

        size_t samplesPerChannel = channelBuf[0].size();
        outData.audioData.resize(samplesPerChannel * channelCount * sizeof(int16_t));
        int16_t* pcm = reinterpret_cast<int16_t*>(outData.audioData.data());

        for (size_t i = 0; i < samplesPerChannel; i++) {
            for (int ch = 0; ch < channelCount; ch++) {
                pcm[i * channelCount + ch] = channelBuf[ch][i];
            }
        }

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = channelCount;
        outData.wfx.nSamplesPerSec = sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = channelCount * 2;
        outData.wfx.nAvgBytesPerSec = sampleRate * channelCount * 2;
        outData.wfx.cbSize = 0;

        outData.hasLoop = hasLoop;
        if (hasLoop && loopByteEnd > 0) {
            outData.loopBegin = loopByteStart;
            outData.loopLength = loopByteEnd - loopByteStart;
            if (outData.loopBegin + outData.loopLength > samplesPerChannel)
                outData.loopLength = (UINT32)(samplesPerChannel - outData.loopBegin);
        }

        return true;
    }

    static bool LoadBrstm(const std::string& filename, WavData& outData) {
        std::vector<uint8_t> d;
        if (!ReadFileIntoMemory(filename, d)) return false;

        if (d.size() < 0x80) return false;
        if (memcmp(d.data(), "RSTM", 4) != 0) return false;

        auto r16 = [&](size_t o) -> uint16_t { return (uint16_t)d[o] << 8 | d[o + 1]; };
        auto r32 = [&](size_t o) -> uint32_t { return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]; };
        auto ri16 = [&](size_t o) -> int16_t { return (int16_t)r16(o); };

        uint32_t hdOff = r32(0x10), dtOff = r32(0x20);

        if (hdOff + 4 > d.size() || memcmp(d.data() + hdOff, "HEAD", 4) != 0) return false;

        uint32_t h1o = r32(hdOff + 0x0C) + 8,
                 h3o = r32(hdOff + 0x1C) + 8;

        uint8_t codec = d[hdOff + h1o];
        bool loop = d[hdOff + h1o + 1] != 0;
        uint8_t chans = d[hdOff + h1o + 2];
        uint32_t srate = r16(hdOff + h1o + 4);
        uint32_t loopStart = r32(hdOff + h1o + 8);
        uint32_t totalSamp = r32(hdOff + h1o + 0x0C);
        uint32_t aOff = r32(hdOff + h1o + 0x10);
        uint32_t numBlk = r32(hdOff + h1o + 0x14);
        uint32_t blkSize = r32(hdOff + h1o + 0x18);
        uint32_t blkSamp = r32(hdOff + h1o + 0x1C);
        uint32_t finBlkSize = r32(hdOff + h1o + 0x20);
        uint32_t finBlkSamp = r32(hdOff + h1o + 0x24);

        if (srate == 0 || chans == 0 || chans > 16) return false;
        if (numBlk == 0 || blkSize == 0 || blkSamp == 0) return false;

        uint32_t dtDataStart = dtOff + 8;
        if (aOff < dtDataStart) aOff += dtDataStart;
        if (aOff < dtDataStart) aOff = dtDataStart;

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = chans;
        outData.wfx.nSamplesPerSec = srate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = chans * 2;
        outData.wfx.nAvgBytesPerSec = srate * chans * 2;
        outData.wfx.cbSize = 0;

        outData.hasLoop = loop && loopStart < totalSamp;
        if (outData.hasLoop) {
            outData.loopBegin = loopStart;
            outData.loopLength = totalSamp - loopStart;
        }

        if (codec == 2) {
            uint8_t h3ch = d[hdOff + h3o];
            if (h3ch > chans) h3ch = chans;

            std::vector<std::vector<int16_t>> coefs(chans, std::vector<int16_t>(16, 0));
            std::vector<int16_t> inh1(chans, 0), inh2(chans, 0);

            for (uint8_t ch = 0; ch < h3ch; ch++) {
                uint32_t cio = r32(hdOff + h3o + 8 + ch * 8);
                cio += 8;
                for (int i = 0; i < 16; i++)
                    coefs[ch][i] = ri16(hdOff + cio + 0x08 + i * 2);
                inh1[ch] = ri16(hdOff + cio + 0x2C);
                inh2[ch] = ri16(hdOff + cio + 0x2E);
            }

            std::vector<std::vector<int16_t>> cbuf(chans);
            for (auto& b : cbuf) b.reserve(totalSamp);

            for (uint32_t b = 0; b < numBlk; b++) {
                size_t bs = (b == numBlk - 1) ? finBlkSize : blkSize;
                size_t bsp = (b == numBlk - 1) ? finBlkSamp : blkSamp;

                for (uint8_t ch = 0; ch < chans; ch++) {
                    size_t base = aOff + b * blkSize * chans + ch * blkSize;
                    if (base >= d.size()) break;
                    if (bs == 0 || bsp == 0) break;

                    size_t remain = std::min<size_t>(bs, d.size() - base);
                    const uint8_t* src = d.data() + base;

                    int32_t yn1 = (b == 0) ? inh1[ch] : cbuf[ch].back();
                    int32_t yn2 = (b == 0) ? inh2[ch] : cbuf[ch][cbuf[ch].size() - 2];

                    size_t consumed = 0;
                    size_t decoded = 0;

                    while (consumed < remain && decoded < bsp) {
                        uint8_t hdr = src[consumed++];
                        int scale = 1 << (hdr & 0x0F);
                        int cix = ((hdr >> 4) & 0x0F) * 2;
                        if (cix > 14) cix = 14;
                        int16_t c1 = coefs[ch][cix];
                        int16_t c2 = coefs[ch][cix + 1];

                        for (int n = 0; n < 14 && decoded < bsp && consumed < remain; n++, decoded++) {
                            int nib = (n & 1) == 0 ? (src[consumed] >> 4) & 0x0F : src[consumed++] & 0x0F;
                            if (nib >= 8) nib -= 16;

                            int32_t samp = ((scale * nib) << 11) + c1 * yn1 + c2 * yn2 + 1024;
                            samp >>= 11;
                            if (samp > 32767) samp = 32767;
                            if (samp < -32768) samp = -32768;

                            yn2 = yn1;
                            yn1 = samp;
                            cbuf[ch].push_back((int16_t)samp);
                        }
                    }
                }
            }

            size_t spc = cbuf[0].size();
            outData.audioData.resize(spc * chans * sizeof(int16_t));
            int16_t* pcm = (int16_t*)outData.audioData.data();
            for (size_t i = 0; i < spc; i++)
                for (uint8_t ch = 0; ch < chans; ch++)
                    pcm[i * chans + ch] = i < cbuf[ch].size() ? cbuf[ch][i] : 0;
            return true;
        }

        if (codec == 1) {
            size_t totalOut = totalSamp;
            outData.audioData.resize(totalOut * chans * sizeof(int16_t));
            int16_t* pcm = (int16_t*)outData.audioData.data();
            size_t pos = 0;
            for (uint32_t b = 0; b < numBlk && pos < totalOut * chans; b++) {
                size_t bsp = (b == numBlk - 1) ? finBlkSamp : blkSamp;
                for (size_t i = 0; i < bsp && pos < totalOut * chans; i++)
                    for (uint8_t ch = 0; ch < chans; ch++) {
                        size_t off = aOff + b * blkSize * chans + ch * blkSize + i * 2;
                        pcm[pos++] = off + 2 <= d.size() ? ri16(off) : 0;
                    }
            }
            return true;
        }

        if (codec == 0) {
            size_t totalOut = totalSamp;
            outData.audioData.resize(totalOut * chans * sizeof(int16_t));
            int16_t* pcm = (int16_t*)outData.audioData.data();
            size_t pos = 0;
            for (uint32_t b = 0; b < numBlk && pos < totalOut * chans; b++) {
                size_t bsp = (b == numBlk - 1) ? finBlkSamp : blkSamp;
                for (size_t i = 0; i < bsp && pos < totalOut * chans; i++)
                    for (uint8_t ch = 0; ch < chans; ch++) {
                        size_t off = aOff + b * blkSize * chans + ch * blkSize + i;
                        pcm[pos++] = off < d.size() ? (int16_t)(int8_t)d[off] * 256 : 0;
                    }
            }
            return true;
        }

        std::cout << "[Brstm] Unsupported codec " << (int)codec << std::endl;
        return false;
    }

#ifdef ENABLE_AAC
    static uint32_t AacReadBE32(const uint8_t* p) {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    }
    static uint16_t AacReadBE16(const uint8_t* p) {
        return ((uint16_t)p[0] << 8) | p[1];
    }
    static uint64_t AacReadBE64(const uint8_t* p) {
        return ((uint64_t)AacReadBE32(p) << 32) | AacReadBE32(p + 4);
    }
    static uint32_t AacReadDescLen(const uint8_t*& p) {
        uint32_t len = 0;
        uint8_t b;
        do { b = *p++; len = (len << 7) | (b & 0x7F); } while (b & 0x80);
        return len;
    }
    static bool AacFindBox(const uint8_t* data, size_t dataSize, size_t start, const char type[4], size_t& outOff, size_t& outSize) {
        size_t pos = start;
        while (pos + 8 <= dataSize) {
            uint32_t sz = AacReadBE32(data + pos);
            if (sz < 8) return false;
            if (sz > dataSize - pos) sz = (uint32_t)(dataSize - pos);
            if (memcmp(data + pos + 4, type, 4) == 0) { outOff = pos; outSize = sz; return true; }
            pos += sz;
        }
        return false;
    }

    static bool LoadAac(const std::string& filename, WavData& outData) {
        std::vector<uint8_t> fileData;
        if (!ReadFileIntoMemory(filename, fileData)) return false;
        if (fileData.size() < 16) return false;

        if ((fileData[0] == 0xFF) && ((fileData[1] & 0xF0) == 0xF0))
            return DecodeAacADTS(fileData, outData);
        else
            return DecodeAacMP4(fileData, outData);
    }

    static bool DecodeAacADTS(const std::vector<uint8_t>& data, WavData& outData) {
        HAACDecoder dec = AACInitDecoder();
        if (!dec) { std::cout << "[AudioLoader] AACInitDecoder failed." << std::endl; return false; }

        std::vector<int16_t> pcm;
        pcm.reserve(data.size() * 2);
        short outbuf[2048 * 2];
        unsigned char* inbuf = (unsigned char*)data.data();
        int bytesLeft = (int)data.size();

        int ret = AACDecode(dec, &inbuf, &bytesLeft, outbuf);
        if (ret != ERR_AAC_NONE) {
            AACFreeDecoder(dec);
            std::cout << "[AudioLoader] AAC decode error: " << ret << std::endl;
            return false;
        }
        AACFrameInfo fi;
        AACGetLastFrameInfo(dec, &fi);
        int nCh = fi.nChans, sr = fi.sampRateOut;
        if (nCh <= 0 || sr <= 0 || fi.outputSamps <= 0) { AACFreeDecoder(dec); return false; }
        pcm.insert(pcm.end(), outbuf, outbuf + fi.outputSamps * nCh);

        while (bytesLeft > 0 && ret == ERR_AAC_NONE) {
            ret = AACDecode(dec, &inbuf, &bytesLeft, outbuf);
            if (ret == ERR_AAC_INDATA_UNDERFLOW) break;
            if (ret != ERR_AAC_NONE) break;
            AACGetLastFrameInfo(dec, &fi);
            pcm.insert(pcm.end(), outbuf, outbuf + fi.outputSamps * nCh);
        }
        AACFreeDecoder(dec);
        if (pcm.empty()) return false;

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)nCh;
        outData.wfx.nSamplesPerSec = sr;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = nCh * 2;
        outData.wfx.nAvgBytesPerSec = sr * nCh * 2;
        outData.wfx.cbSize = 0;
        outData.audioData.resize(pcm.size() * sizeof(int16_t));
        memcpy(outData.audioData.data(), pcm.data(), pcm.size() * sizeof(int16_t));
        return true;
    }

    static bool DecodeAacMP4(const std::vector<uint8_t>& data, WavData& outData) {
        const uint8_t* raw = data.data();
        size_t sz = data.size();

        size_t moovOff, moovSize;
        if (!AacFindBox(raw, sz, 0, "moov", moovOff, moovSize)) {
            std::cout << "[AudioLoader] No moov box in M4A." << std::endl; return false;
        }

        int nCh = 0, sr = 0, profile = AAC_PROFILE_LC;
        std::vector<uint32_t> sampleSizes;
        std::vector<uint32_t> chunkOffsets;
        std::vector<uint32_t> stscData;

        // Walk trak boxes inside moov
        size_t pos = moovOff + 8, moovEnd = moovOff + moovSize;
        while (pos + 8 < moovEnd) {
            uint32_t trakSize = AacReadBE32(raw + pos);
            if (trakSize < 8 || pos + trakSize > sz) break;
            if (memcmp(raw + pos + 4, "trak", 4) == 0) {
                size_t trakEnd = pos + trakSize;
                size_t mdiaOff = pos + 8;
                size_t mdiaSize;
                if (!AacFindBox(raw, sz, mdiaOff, "mdia", mdiaOff, mdiaSize)) { pos += trakSize; continue; }
                size_t hdlrOff = mdiaOff + 8, hdlrSize;
                if (!AacFindBox(raw, sz, hdlrOff, "hdlr", hdlrOff, hdlrSize)) { pos += trakSize; continue; }
                const uint8_t* hdlrData = raw + hdlrOff + 12;
                if (hdlrData + 4 > raw + sz || memcmp(hdlrData, "soun", 4) != 0) { pos += trakSize; continue; }

                size_t minfOff = mdiaOff + 8, minfSize;
                if (!AacFindBox(raw, sz, minfOff, "minf", minfOff, minfSize)) { pos += trakSize; continue; }
                size_t stblOff = minfOff + 8, stblSize;
                if (!AacFindBox(raw, sz, stblOff, "stbl", stblOff, stblSize)) { pos += trakSize; continue; }
                size_t stblEnd = stblOff + stblSize;

                size_t stsdOff = stblOff + 8, stsdSize;
                if (AacFindBox(raw, sz, stsdOff, "stsd", stsdOff, stsdSize)) {
                    const uint8_t* stsd = raw + stsdOff + 12;
                    uint32_t numDesc = AacReadBE32(stsd);
                    if (numDesc >= 1) {
                        const uint8_t* entry = stsd + 4;
                        uint32_t entrySize = AacReadBE32(entry);
                        if (entrySize >= 28 && memcmp(entry + 4, "mp4a", 4) == 0) {
                            nCh = AacReadBE16(entry + 16);
                            sr = AacReadBE32(entry + 24) >> 16;
                            size_t esdsOff = entry - raw + 28, esdsSize;
                            if (AacFindBox(raw, sz, esdsOff, "esds", esdsOff, esdsSize)) {
                                const uint8_t* esds = raw + esdsOff + 12;
                                const uint8_t* esdsEnd = esds + esdsSize - 12;
                                while (esds < esdsEnd) {
                                    if (*esds == 0x05) {
                                        esds++;
                                        uint32_t dsiLen = AacReadDescLen(esds);
                                        if (dsiLen >= 2 && esds + dsiLen <= raw + sz) {
                                            int sri = ((esds[0] & 0x07) << 1) | ((esds[1] >> 7) & 0x01);
                                            int cc = (esds[1] >> 3) & 0x0F;
                                            static const int aacSR[] = {96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
                                            if (sri >= 0 && sri < 13) sr = aacSR[sri];
                                            if (cc > 0) nCh = cc;
                                        }
                                        break;
                                    }
                                    if (*esds == 0x03 || *esds == 0x04) { esds++; AacReadDescLen(esds); }
                                    else esds++;
                                }
                            }
                        }
                    }
                }

                size_t stszOff = stblOff + 8, stszSize;
                if (AacFindBox(raw, sz, stszOff, "stsz", stszOff, stszSize)) {
                    const uint8_t* stsz = raw + stszOff + 12;
                    uint32_t constSize = AacReadBE32(stsz);
                    uint32_t numSamp = AacReadBE32(stsz + 4);
                    sampleSizes.reserve(numSamp);
                    if (constSize == 0) {
                        for (uint32_t i = 0; i < numSamp; i++)
                            sampleSizes.push_back(AacReadBE32(stsz + 8 + i * 4));
                    } else {
                        for (uint32_t i = 0; i < numSamp; i++)
                            sampleSizes.push_back(constSize);
                    }
                }

                size_t stcoOff = stblOff + 8, stcoSize;
                if (AacFindBox(raw, sz, stcoOff, "stco", stcoOff, stcoSize)) {
                    const uint8_t* stco = raw + stcoOff + 12;
                    uint32_t numChunks = AacReadBE32(stco);
                    chunkOffsets.reserve(numChunks);
                    for (uint32_t i = 0; i < numChunks; i++)
                        chunkOffsets.push_back(AacReadBE32(stco + 4 + i * 4));
                } else if (AacFindBox(raw, sz, stblOff + 8, "co64", stcoOff, stcoSize)) {
                    const uint8_t* stco = raw + stcoOff + 12;
                    uint32_t numChunks = AacReadBE32(stco);
                    chunkOffsets.reserve(numChunks);
                    for (uint32_t i = 0; i < numChunks; i++)
                        chunkOffsets.push_back((uint32_t)AacReadBE64(stco + 4 + i * 8));
                }

                size_t stscOff = stblOff + 8, stscSize;
                if (AacFindBox(raw, sz, stscOff, "stsc", stscOff, stscSize)) {
                    const uint8_t* stsc = raw + stscOff + 12;
                    uint32_t numEnt = AacReadBE32(stsc);
                    stscData.reserve(numEnt * 3);
                    for (uint32_t i = 0; i < numEnt; i++) {
                        stscData.push_back(AacReadBE32(stsc + 4 + i * 12));
                        stscData.push_back(AacReadBE32(stsc + 4 + i * 12 + 4));
                        stscData.push_back(AacReadBE32(stsc + 4 + i * 12 + 8));
                    }
                }
            }
            pos += trakSize;
        }

        if (nCh <= 0 || sr <= 0 || sampleSizes.empty() || chunkOffsets.empty()) {
            std::cout << "[AudioLoader] Invalid M4A file." << std::endl; return false;
        }

        struct AacSample { uint32_t off; uint32_t sz; };
        std::vector<AacSample> samples;
        samples.reserve(sampleSizes.size());
        uint32_t chunkIdx = 0, sampleIdx = 0;
        for (size_t e = 0; e + 3 <= stscData.size() && chunkIdx < (uint32_t)chunkOffsets.size(); e += 3) {
            uint32_t first = stscData[e];
            uint32_t perChunk = stscData[e + 1];
            uint32_t last = (e + 6 <= stscData.size()) ? stscData[e + 3] : (uint32_t)(chunkOffsets.size() + 1);
            for (uint32_t c = first; c < last && chunkIdx < (uint32_t)chunkOffsets.size(); c++) {
                uint32_t off = chunkOffsets[chunkIdx];
                for (uint32_t s = 0; s < perChunk && sampleIdx < (uint32_t)sampleSizes.size(); s++, sampleIdx++) {
                    samples.push_back({off, sampleSizes[sampleIdx]});
                    off += sampleSizes[sampleIdx];
                }
                chunkIdx++;
            }
        }
        if (samples.empty()) {
            for (uint32_t i = 0; i < chunkOffsets.size() && i < sampleSizes.size(); i++)
                samples.push_back({chunkOffsets[i], sampleSizes[i]});
        }

        HAACDecoder dec = AACInitDecoder();
        if (!dec) { std::cout << "[AudioLoader] AACInitDecoder failed." << std::endl; return false; }

        AACFrameInfo rInfo;
        memset(&rInfo, 0, sizeof(rInfo));
        rInfo.nChans = nCh;
        rInfo.sampRateCore = sr;
        rInfo.sampRateOut = sr;
        rInfo.profile = profile;
        rInfo.bitsPerSample = 16;
        rInfo.outputSamps = 1024;
        if (AACSetRawBlockParams(dec, 0, &rInfo) != ERR_AAC_NONE) {
            AACFreeDecoder(dec);
            std::cout << "[AudioLoader] AACSetRawBlockParams failed." << std::endl; return false;
        }

        std::vector<int16_t> pcm;
        pcm.reserve(samples.size() * 1024 * nCh);
        short outbuf[2048 * 2];
        for (size_t i = 0; i < samples.size(); i++) {
            uint32_t off = samples[i].off;
            uint32_t len = samples[i].sz;
            if (off + len > sz) continue;
            unsigned char* inbuf = (unsigned char*)raw + off;
            int rem = (int)len;
            int ret = AACDecode(dec, &inbuf, &rem, outbuf);
            if (ret == ERR_AAC_NONE) {
                AACFrameInfo fi;
                AACGetLastFrameInfo(dec, &fi);
                pcm.insert(pcm.end(), outbuf, outbuf + fi.outputSamps * nCh);
            }
        }
        AACFreeDecoder(dec);
        if (pcm.empty()) return false;

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)nCh;
        outData.wfx.nSamplesPerSec = sr;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = nCh * 2;
        outData.wfx.nAvgBytesPerSec = sr * nCh * 2;
        outData.wfx.cbSize = 0;
        outData.audioData.resize(pcm.size() * sizeof(int16_t));
        memcpy(outData.audioData.data(), pcm.data(), pcm.size() * sizeof(int16_t));
        return true;
    }
#endif
};
