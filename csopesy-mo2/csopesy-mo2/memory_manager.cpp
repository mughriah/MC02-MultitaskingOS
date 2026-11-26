#include "memory_manager.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>

void MemoryManager::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex);
    frameSize = config.memPerFrame;
    const uint64_t frameCount = config.maxOverallMem / config.memPerFrame;
    physical.assign(static_cast<size_t>(config.maxOverallMem), 0);
    frames.assign(static_cast<size_t>(frameCount), Frame{});
    processes.clear();
    backingStore.clear();
    useClock = pagedIn = pagedOut = pageFaults = 0;
    storeDirty = true;
    writeStoreFile();
}

void MemoryManager::registerProcess(int pid, const std::string& name, uint64_t memorySize) {
    std::lock_guard<std::mutex> lock(mutex);
    if (frameSize == 0) { return; }
    ProcessInfo info;
    info.name = name;
    info.memorySize = memorySize;
    const uint64_t pages = (memorySize + frameSize - 1) / frameSize;
    info.pageTable.assign(static_cast<size_t>(pages), -1);
    processes[pid] = std::move(info);
}

void MemoryManager::releaseProcess(int pid) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!processes.count(pid)) { return; }
    for (auto& frame : frames) {
        if (frame.pid == pid) { frame = Frame{}; }
    }
    auto it = backingStore.lower_bound({ pid, 0 });
    while (it != backingStore.end() && it->first.first == pid) {
        it = backingStore.erase(it);
        storeDirty = true;
    }
    processes.erase(pid);
}

int64_t MemoryManager::pickVictim() const {
    // Least recently used among occupied frames.
    int64_t victim = -1;
    uint64_t oldest = std::numeric_limits<uint64_t>::max();
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].pid >= 0 && frames[i].lastUse < oldest) {
            oldest = frames[i].lastUse;
            victim = static_cast<int64_t>(i);
        }
    }
    return victim;
}

void MemoryManager::evict(size_t frameIndex) {
    Frame& frame = frames[frameIndex];
    auto owner = processes.find(frame.pid);
    const uint8_t* source = &physical[frameIndex * frameSize];
    // Page-out: the frame's bytes are stored in the backing store.
    backingStore[{ frame.pid, frame.page }] = std::vector<uint8_t>(source, source + frameSize);
    if (owner != processes.end() && frame.page < owner->second.pageTable.size()) {
        owner->second.pageTable[static_cast<size_t>(frame.page)] = -1;
    }
    frame = Frame{};
    ++pagedOut;
    storeDirty = true;
}

int64_t MemoryManager::ensureResident(int pid, uint64_t page, bool& faulted) {
    auto owner = processes.find(pid);
    if (owner == processes.end()) { return -1; }
    auto& table = owner->second.pageTable;
    if (page >= table.size()) { return -1; }
    if (table[static_cast<size_t>(page)] >= 0) {
        const int64_t hit = table[static_cast<size_t>(page)];
        frames[static_cast<size_t>(hit)].lastUse = ++useClock;
        return hit;
    }

    // ---- page fault ----
    faulted = true;
    ++pageFaults;

    int64_t target = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (frames[i].pid < 0) { target = static_cast<int64_t>(i); break; }
    }
    if (target < 0) {
        // No free frame: the page replacement algorithm (LRU) picks a victim.
        target = pickVictim();
        if (target < 0) { return -1; }
        evict(static_cast<size_t>(target));
    }

    // Bring the page in from the backing store (or zero-fill on first touch).
    uint8_t* destination = &physical[static_cast<size_t>(target) * frameSize];
    auto stored = backingStore.find({ pid, page });
    if (stored != backingStore.end()) {
        std::memcpy(destination, stored->second.data(), static_cast<size_t>(frameSize));
        backingStore.erase(stored);
        storeDirty = true;
    }
    else {
        std::memset(destination, 0, static_cast<size_t>(frameSize));
    }
    frames[static_cast<size_t>(target)] = Frame{ pid, page, ++useClock };
    table[static_cast<size_t>(page)] = target;
    ++pagedIn;
    return target;
}

bool MemoryManager::touchSymbol(int pid, int slot) {
    std::lock_guard<std::mutex> lock(mutex);
    if (frameSize == 0 || slot < 0) { return false; }
    bool faulted = false;
    ensureResident(pid, (static_cast<uint64_t>(slot) * 2) / frameSize, faulted);
    return faulted;
}

uint16_t MemoryManager::read16(int pid, uint64_t address, bool* faulted) {
    std::lock_guard<std::mutex> lock(mutex);
    bool fault = false;
    uint16_t value = 0;
    auto owner = processes.find(pid);
    if (frameSize != 0 && owner != processes.end()) {
        for (uint64_t i = 0; i < 2; ++i) {
            const uint64_t a = address + i;
            if (a >= owner->second.memorySize) { break; }
            const int64_t frame = ensureResident(pid, a / frameSize, fault);
            if (frame < 0) { break; }
            const uint8_t byte = physical[static_cast<size_t>(frame) * frameSize + a % frameSize];
            value = static_cast<uint16_t>(value | (byte << (8 * i)));
        }
    }
    if (faulted) { *faulted = fault; }
    return value;
}

void MemoryManager::write16(int pid, uint64_t address, uint16_t value, bool* faulted) {
    std::lock_guard<std::mutex> lock(mutex);
    bool fault = false;
    auto owner = processes.find(pid);
    if (frameSize != 0 && owner != processes.end()) {
        for (uint64_t i = 0; i < 2; ++i) {
            const uint64_t a = address + i;
            if (a >= owner->second.memorySize) { break; }
            const int64_t frame = ensureResident(pid, a / frameSize, fault);
            if (frame < 0) { break; }
            physical[static_cast<size_t>(frame) * frameSize + a % frameSize] =
                static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
        }
    }
    if (faulted) { *faulted = fault; }
}

MemoryStats MemoryManager::getStats() const {
    std::lock_guard<std::mutex> lock(mutex);
    MemoryStats stats;
    stats.frameSize = frameSize;
    stats.totalFrames = frames.size();
    for (const auto& frame : frames) {
        if (frame.pid >= 0) { ++stats.usedFrames; }
    }
    stats.totalBytes = stats.totalFrames * frameSize;
    stats.usedBytes = stats.usedFrames * frameSize;
    stats.freeBytes = stats.totalBytes - stats.usedBytes;
    stats.pagedIn = pagedIn;
    stats.pagedOut = pagedOut;
    stats.pageFaults = pageFaults;
    stats.swappedPages = backingStore.size();
    return stats;
}

std::map<int, uint64_t> MemoryManager::residentBytesByProcess() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<int, uint64_t> result;
    for (const auto& frame : frames) {
        if (frame.pid >= 0) { result[frame.pid] += frameSize; }
    }
    return result;
}

void MemoryManager::flushBackingStore() {
    std::lock_guard<std::mutex> lock(mutex);
    if (storeDirty) { writeStoreFile(); }
}

void MemoryManager::writeStoreFile() {
    storeDirty = false;
    std::ofstream file(backingStorePath, std::ios::trunc);
    if (!file) { return; }
    file << "# CSOPESY backing store: pages currently paged out of physical memory\n"
         << "# frame size: " << frameSize << " bytes | pages stored: " << backingStore.size() << '\n';
    for (const auto& entry : backingStore) {
        const int pid = entry.first.first;
        const auto owner = processes.find(pid);
        file << "pid=" << pid
             << " process=" << (owner != processes.end() ? owner->second.name : "?")
             << " page=" << entry.first.second
             << " bytes=" << entry.second.size() << " data=";
        const bool allZero = std::all_of(entry.second.begin(), entry.second.end(),
                                         [](uint8_t b) { return b == 0; });
        if (allZero) { file << "(all zero)"; }
        else {
            file << std::hex << std::uppercase << std::setfill('0');
            for (uint8_t byte : entry.second) { file << std::setw(2) << static_cast<int>(byte); }
            file << std::dec << std::setfill(' ');
        }
        file << '\n';
    }
}
