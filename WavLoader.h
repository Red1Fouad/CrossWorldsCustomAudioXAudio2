#pragma once
#include <windows.h>
#include <fstream>
#include <vector>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cstdlib>

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

struct WavData {
    WAVEFORMATEX wfx;
    std::vector<BYTE> audioData;
    bool hasLoop = false;
    UINT32 loopBegin = 0;
    UINT32 loopLength = 0;
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
        else {
            return LoadWav(filename, outData);
        }
    }

private:
    static bool LoadWav(const std::string& filename, WavData& outData) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cout << "[WavLoader] Error: Could not open file handle for: " << filename << std::endl;
            return false;
        }

        char chunkId[4];
        file.read(chunkId, 4);
        if (file.gcount() < 4 || strncmp(chunkId, "RIFF", 4) != 0) {
            std::cout << "[WavLoader] Error: Invalid RIFF header." << std::endl;
            return false;
        }

        file.seekg(4, std::ios::cur);

        char format[4];
        file.read(format, 4);
        if (file.gcount() < 4 || strncmp(format, "WAVE", 4) != 0) {
            std::cout << "[WavLoader] Error: Invalid WAVE header." << std::endl;
            return false;
        }

        bool foundFmt = false;
        bool foundData = false;

        while (file.good() && (!foundFmt || !foundData)) {
            char subChunkId[4];
            file.read(subChunkId, 4);
            if (file.gcount() < 4) break;

            uint32_t subChunkSize;
            file.read(reinterpret_cast<char*>(&subChunkSize), 4);
            if (file.gcount() < 4) break;

            if (strncmp(subChunkId, "fmt ", 4) == 0) {
                if (subChunkSize >= 18) {
                    file.read(reinterpret_cast<char*>(&outData.wfx), 18);
                    int remaining = subChunkSize - 18;
                    if (remaining > 0) file.seekg(remaining, std::ios::cur);
                } else {
                    file.read(reinterpret_cast<char*>(&outData.wfx), 16);
                    outData.wfx.cbSize = 0;
                    int remaining = subChunkSize - 16;
                    if (remaining > 0) file.seekg(remaining, std::ios::cur);
                }
                foundFmt = true;
            }
            else if (strncmp(subChunkId, "data", 4) == 0) {
                outData.audioData.resize(subChunkSize);
                file.read(reinterpret_cast<char*>(outData.audioData.data()), subChunkSize);
                foundData = true;
            }
            else {
                file.seekg(subChunkSize, std::ios::cur);
            }

            if (subChunkSize % 2 != 0) {
                file.seekg(1, std::ios::cur);
            }
        }

        if (!foundFmt) std::cout << "[WavLoader] Error: 'fmt ' chunk not found." << std::endl;
        if (!foundData) std::cout << "[WavLoader] Error: 'data' chunk not found." << std::endl;

        return foundFmt && foundData;
    }

    static bool LoadMp3(const std::string& filename, WavData& outData) {
#ifdef ENABLE_MP3
        drmp3_config config;
        drmp3_uint64 totalPCMFrameCount;

        drmp3_int16* pSampleData = drmp3_open_file_and_read_pcm_frames_s16(filename.c_str(), &config, &totalPCMFrameCount, NULL);

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
        int channels, sampleRate;
        short* output;

        int samples = stb_vorbis_decode_filename(filename.c_str(), &channels, &sampleRate, &output);

        if (samples == -1) {
            std::cout << "[AudioLoader] Error: Failed to load OGG: " << filename << std::endl;
            return false;
        }

        outData.wfx.wFormatTag = WAVE_FORMAT_PCM;
        outData.wfx.nChannels = (WORD)channels;
        outData.wfx.nSamplesPerSec = sampleRate;
        outData.wfx.wBitsPerSample = 16;
        outData.wfx.nBlockAlign = outData.wfx.nChannels * 2;
        outData.wfx.nAvgBytesPerSec = outData.wfx.nSamplesPerSec * outData.wfx.nBlockAlign;
        outData.wfx.cbSize = 0;

        size_t dataSize = samples * channels * sizeof(short);
        outData.audioData.resize(dataSize);
        memcpy(outData.audioData.data(), output, dataSize);

        free(output);
        return true;
#else
        return false;
#endif
    }

    static bool LoadFlac(const std::string& filename, WavData& outData) {
#ifdef ENABLE_FLAC
        unsigned int channels;
        unsigned int sampleRate;
        drflac_uint64 totalPCMFrameCount;

        drflac_int16* pSampleData = drflac_open_file_and_read_pcm_frames_s16(filename.c_str(), &channels, &sampleRate, &totalPCMFrameCount, NULL);

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
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;

        std::vector<uint8_t> fileData((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

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
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) return false;
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

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
};
