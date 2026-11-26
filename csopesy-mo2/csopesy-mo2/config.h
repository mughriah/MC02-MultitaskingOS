#pragma once
#include <cstdint>
#include <string>

// Memory sizes in config.txt and in "screen -s/-c" must be a power of 2 in [2^6, 2^16].
inline bool isValidMemorySize(uint64_t n) {
    return n >= 64 && n <= 65536 && (n & (n - 1)) == 0;
}

struct Config {
    // ---- MO1 parameters ----
    int numCPU = 4;
    std::string scheduler = "rr";
    uint64_t quantumCycles = 5;
    uint64_t batchProcessFreq = 10;
    uint64_t minInstructions = 5;
    uint64_t maxInstructions = 15;
    uint64_t delaysPerExec = 0;

    // ---- MO2 parameters (all power of 2, [2^6, 2^16]) ----
    uint64_t maxOverallMem = 16384;   // max-overall-mem : total physical memory in bytes
    uint64_t memPerFrame = 256;       // mem-per-frame   : bytes per frame (= bytes per page)
    uint64_t minMemPerProc = 256;     // min-mem-per-proc: lower bound for scheduler-start processes
    uint64_t maxMemPerProc = 4096;    // max-mem-per-proc: upper bound for scheduler-start processes

    bool loadFromFile(const std::string& filename, std::string* error = nullptr);
};
