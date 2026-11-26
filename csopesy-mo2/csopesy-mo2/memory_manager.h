#pragma once

#include "config.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct MemoryStats {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    uint64_t freeBytes = 0;
    uint64_t frameSize = 0;
    uint64_t totalFrames = 0;
    uint64_t usedFrames = 0;
    uint64_t pagedIn = 0;      // accumulated pages loaded into frames
    uint64_t pagedOut = 0;     // accumulated pages evicted to the backing store
    uint64_t pageFaults = 0;
    uint64_t swappedPages = 0; // pages currently sitting in the backing store
};

// Demand-paging memory manager.
//  * Physical memory = max-overall-mem bytes split into frames of mem-per-frame bytes.
//  * Each process owns a page table (one entry per page, pages = ceil(size / frame size)).
//    Nothing is loaded until the process touches it: that touch is a page fault.
//  * When no frame is free, the least-recently-used frame is evicted: its bytes are copied
//    to the backing store and the owner's page-table entry is cleared.
//  * The backing store is mirrored to "csopesy-backing-store.txt".
// Thread-safe: has its own mutex and never calls back into Process or Scheduler.
class MemoryManager {
public:
    void initialize(const Config& config);

    void registerProcess(int pid, const std::string& name, uint64_t memorySize);
    void releaseProcess(int pid);   // frees frames and backing-store pages of a finished process

    // Makes sure the page holding symbol-table slot 'slot' is resident. True if it faulted.
    bool touchSymbol(int pid, int slot);

    // 2-byte little-endian access at a virtual address. The caller has already checked that
    // the address is inside the process; each byte faults its own page in if necessary.
    uint16_t read16(int pid, uint64_t address, bool* faulted = nullptr);
    void write16(int pid, uint64_t address, uint16_t value, bool* faulted = nullptr);

    MemoryStats getStats() const;
    std::map<int, uint64_t> residentBytesByProcess() const;

    // Rewrites csopesy-backing-store.txt if anything changed since the last flush.
    void flushBackingStore();

private:
    struct Frame {
        int pid = -1;
        uint64_t page = 0;
        uint64_t lastUse = 0;
    };
    struct ProcessInfo {
        std::string name;
        uint64_t memorySize = 0;
        std::vector<int64_t> pageTable;   // frame index, or -1 when not resident
    };

    mutable std::mutex mutex;
    uint64_t frameSize = 0;
    std::vector<uint8_t> physical;
    std::vector<Frame> frames;
    std::map<int, ProcessInfo> processes;
    std::map<std::pair<int, uint64_t>, std::vector<uint8_t>> backingStore;
    uint64_t useClock = 0;
    uint64_t pagedIn = 0;
    uint64_t pagedOut = 0;
    uint64_t pageFaults = 0;
    bool storeDirty = false;
    std::string backingStorePath = "csopesy-backing-store.txt";

    int64_t ensureResident(int pid, uint64_t page, bool& faulted);
    int64_t pickVictim() const;
    void evict(size_t frame);
    void writeStoreFile();
};
