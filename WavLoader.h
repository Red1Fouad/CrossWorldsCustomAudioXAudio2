#pragma once
#include <windows.h>
#include <fstream>
#include <vector>
#include <iostream>
#include <cstring>

struct WavData {
    WAVEFORMATEX wfx;
    std::vector<BYTE> audioData;
};

class WavLoader {
public:
    static bool LoadWav(const std::string& filename, WavData& outData) {
        std::ifstream file(filename, std::ios::binary);
        if (!file.is_open()) {
            std::cout << "[WavLoader] Error: Could not open file handle for: " << filename << std::endl;
            return false;
        }

        // RIFF Header
        char chunkId[4];
        file.read(chunkId, 4);
        if (file.gcount() < 4 || strncmp(chunkId, "RIFF", 4) != 0) {
            std::cout << "[WavLoader] Error: Invalid RIFF header." << std::endl;
            return false;
        }

        file.seekg(4, std::ios::cur); // Skip ChunkSize

        char format[4];
        file.read(format, 4);
        if (file.gcount() < 4 || strncmp(format, "WAVE", 4) != 0) {
            std::cout << "[WavLoader] Error: Invalid WAVE header." << std::endl;
            return false;
        }

        // Find "fmt " and "data" chunks
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
                // Read format
                // Handle 16-byte (PCM) vs 18-byte+ (Extensible) fmt chunks
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
                // Read data
                outData.audioData.resize(subChunkSize);
                file.read(reinterpret_cast<char*>(outData.audioData.data()), subChunkSize);
                foundData = true;
            }
            else {
                // Skip unknown chunk
                char name[5] = { 0 };
                memcpy(name, subChunkId, 4);
                std::cout << "[WavLoader] Debug: Skipping chunk '" << name << "' (" << subChunkSize << " bytes)" << std::endl;
                file.seekg(subChunkSize, std::ios::cur);
            }

            // RIFF standard requires chunks to be word-aligned. 
            // If size is odd, there is a padding byte.
            if (subChunkSize % 2 != 0) {
                file.seekg(1, std::ios::cur);
            }
        }

        if (!foundFmt) std::cout << "[WavLoader] Error: 'fmt ' chunk not found." << std::endl;
        if (!foundData) std::cout << "[WavLoader] Error: 'data' chunk not found." << std::endl;

        return foundFmt && foundData;
    }
};
