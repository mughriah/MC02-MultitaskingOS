#include "scheduler.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

Scheduler::Scheduler() {
}

Scheduler::~Scheduler() {
    shutdownRequested = true;
    batchGenerationEnabled = false;

    if (worker.joinable()) {
        worker.join();
    }
    memory.flushBackingStore();
}

bool Scheduler::initialize(const Config& loadedConfig) {
    std::lock_guard<std::mutex> lock(mutex);

    if (worker.joinable()) {
        return false;
    }

    config = loadedConfig;
    cores.clear();
    cores.resize(static_cast<size_t>(config.numCPU));

    currentTick = 0;
    busyCoreTicks = 0;
    nextProcessNumber = 1;
    nextBatchTick = 0;

    processes.clear();
    readyQueue.clear();

    shutdownRequested = false;
    batchGenerationEnabled = false;

    sleeping.clear();

    memory.initialize(config);

    return true;
}

void Scheduler::start() {
    if (worker.joinable()) {
        return;
    }

    shutdownRequested = false;
    worker = std::thread(&Scheduler::run, this);
}

void Scheduler::stop() {
    shutdownRequested = true;
    batchGenerationEnabled = false;

    if (worker.joinable()) {
        worker.join();
    }
    memory.flushBackingStore();
}

void Scheduler::startBatchGeneration() {
    std::lock_guard<std::mutex> lock(mutex);
    if (batchGenerationEnabled) { return; }
    nextBatchTick = currentTick + config.batchProcessFreq;
    batchGenerationEnabled = true;
}

void Scheduler::stopBatchGeneration() {
    std::lock_guard<std::mutex> lock(mutex);
    batchGenerationEnabled = false;
}

// Caller holds 'mutex'. Registers the process with the memory manager and queues it.
std::shared_ptr<Process> Scheduler::addProcessLocked(
    const std::string& processName,
    uint64_t memorySize,
    std::vector<Instruction> program
) {
    const int id = static_cast<int>(processes.size() + 1);

    auto process = std::make_shared<Process>(
        id,
        processName,
        std::move(program),
        currentTick,
        memorySize
    );

    memory.registerProcess(id, processName, memorySize);
    processes[processName] = process;
    readyQueue.push_back(process);
    return process;
}

std::shared_ptr<Process> Scheduler::createProcess(
    const std::string& processName,
    uint64_t memorySize
) {
    std::lock_guard<std::mutex> lock(mutex);

    if (processName.empty() ||
        processes.find(processName) != processes.end()) {
        return nullptr;
    }

    std::uniform_int_distribution<uint64_t> countDistribution(
        config.minInstructions,
        config.maxInstructions
    );

    static thread_local std::mt19937 rng(std::random_device{}());

    const uint64_t count = countDistribution(rng);
    return addProcessLocked(processName, memorySize,
        generateInstructions(processName, count, memorySize));
}

std::shared_ptr<Process> Scheduler::createProcess(
    const std::string& processName,
    uint64_t memorySize,
    std::vector<Instruction> program
) {
    std::lock_guard<std::mutex> lock(mutex);

    if (processName.empty() ||
        processes.find(processName) != processes.end()) {
        return nullptr;
    }
    return addProcessLocked(processName, memorySize, std::move(program));
}

std::shared_ptr<Process> Scheduler::findProcess(
    const std::string& processName
) const {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = processes.find(processName);

    if (it == processes.end()) {
        return nullptr;
    }

    // Return active and finished processes.
    // This allows process-smi to display final information.
    return it->second;
}

std::vector<std::shared_ptr<Process>>
Scheduler::getAllProcesses() const {
    std::lock_guard<std::mutex> lock(mutex);

    std::vector<std::shared_ptr<Process>> result;
    result.reserve(processes.size());

    for (const auto& entry : processes) {
        result.push_back(entry.second);
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const auto& a, const auto& b) {
            return a->getID() < b->getID();
        }
    );

    return result;
}

uint64_t Scheduler::getCurrentTick() const {
    std::lock_guard<std::mutex> lock(mutex);
    return currentTick;
}

uint64_t Scheduler::getBusyCoreTicks() const {
    std::lock_guard<std::mutex> lock(mutex);
    return busyCoreTicks;
}

int Scheduler::getNumCPU() const {
    std::lock_guard<std::mutex> lock(mutex);
    return config.numCPU;
}

int Scheduler::getAvailableCores() const {
    std::lock_guard<std::mutex> lock(mutex);

    int available = 0;

    for (const auto& core : cores) {
        if (!core.process) {
            ++available;
        }
    }

    return available;
}

Config Scheduler::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex);
    return config;
}

SchedulerSnapshot Scheduler::getSnapshot() const {
    // Keep the scheduler locked while copying every process, so one report
    // cannot show the same process in both the active and finished sections.
    std::lock_guard<std::mutex> lock(mutex);
    SchedulerSnapshot snapshot;
    snapshot.tick = currentTick;
    snapshot.busyCoreTicks = busyCoreTicks;
    snapshot.cpuCount = config.numCPU;
    snapshot.totalCoreTicks = currentTick * static_cast<uint64_t>(config.numCPU);
    snapshot.idleCoreTicks = snapshot.totalCoreTicks > busyCoreTicks
        ? snapshot.totalCoreTicks - busyCoreTicks : 0;
    for (const auto& core : cores) {
        if (core.process) { ++snapshot.usedCores; }
    }
    for (const auto& entry : processes) {
        snapshot.processes.push_back(entry.second->getSnapshot());
    }
    std::sort(snapshot.processes.begin(), snapshot.processes.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
        });
    for (const auto& p : snapshot.processes) {
        if (p.state == ProcessState::Finished) { ++snapshot.finishedProcesses; }
        else if (p.state == ProcessState::Running) { ++snapshot.activeProcesses; }
        else { ++snapshot.inactiveProcesses; }
    }
    return snapshot;
}

MemoryStats Scheduler::getMemoryStats() const {
    return memory.getStats();
}

std::map<int, uint64_t> Scheduler::getResidentBytes() const {
    return memory.residentBytesByProcess();
}

// Random power-of-2 size between min-mem-per-proc and max-mem-per-proc (inclusive).
uint64_t Scheduler::rollMemorySize() const {
    auto exponent = [](uint64_t v) {
        int e = 0;
        while ((uint64_t{ 1 } << e) < v) { ++e; }
        return e;
        };
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> pick(exponent(config.minMemPerProc), exponent(config.maxMemPerProc));
    return uint64_t{ 1 } << pick(rng);
}

void Scheduler::generateBatchProcess() {
    std::ostringstream name;

    name << 'p'
        << std::setw(2)
        << std::setfill('0')
        << nextProcessNumber++;

    while (processes.find(name.str()) != processes.end()) {
        name.str("");
        name.clear();

        name << 'p'
            << std::setw(2)
            << std::setfill('0')
            << nextProcessNumber++;
    }

    std::uniform_int_distribution<uint64_t> countDistribution(
        config.minInstructions,
        config.maxInstructions
    );

    static thread_local std::mt19937 rng(std::random_device{}());

    const uint64_t count = countDistribution(rng);
    const uint64_t memorySize = rollMemorySize();

    addProcessLocked(name.str(), memorySize,
        generateInstructions(name.str(), count, memorySize));
}

void Scheduler::tick() {
    std::lock_guard<std::mutex> lock(mutex);

    ++currentTick;

    // An expired quantum yields before the next tick's instruction. Keeping
    // the core assigned until then also makes screen -ls show current usage.
    for (auto& core : cores) {
        if (core.process && config.scheduler == "rr" && core.quantumUsed >= config.quantumCycles) {
            core.process->setReady();
            readyQueue.push_back(core.process);
            core.process.reset();
            core.quantumUsed = 0;
        }
    }

    // Advance sleeping processes and requeue those that wake.
    for (auto it = sleeping.begin(); it != sleeping.end();) {
        (*it)->advanceSleep(currentTick);
        const ProcessState state = (*it)->getState();
        if (state == ProcessState::Ready) { readyQueue.push_back(*it); }
        if (state == ProcessState::Finished) { memory.releaseProcess((*it)->getID()); }
        if (state == ProcessState::Sleeping) { ++it; }
        else { it = sleeping.erase(it); }
    }

    // Assign ready processes to idle CPU cores.
    dispatch();

    // Execute one CPU tick on every occupied core. Memory is only touched here, i.e. only
    // while a process holds a CPU worker.
    for (auto& core : cores) {
        if (!core.process) {
            continue;
        }

        auto process = core.process;

        StepResult result = process->executeOne(
            currentTick,
            config.delaysPerExec,
            memory
        );

        ++busyCoreTicks;
        ++core.quantumUsed;

        if (result == StepResult::Finished ||
            result == StepResult::Sleeping) {
            if (result == StepResult::Sleeping) { sleeping.push_back(core.process); }
            // Finished (normally or by an access violation): free frames + backing-store pages.
            else { memory.releaseProcess(process->getID()); }
            core.process.reset();
            core.quantumUsed = 0;
            continue;
        }

    }

    // Generate a process at the configured CPU-tick frequency.
    if (batchGenerationEnabled &&
        config.batchProcessFreq > 0 &&
        currentTick >= nextBatchTick) {
        generateBatchProcess();
        nextBatchTick = currentTick + config.batchProcessFreq;
    }

    // Refill cores freed this tick so screen -ls reflects true utilization.
    dispatch();

    // Keep csopesy-backing-store.txt current.
    memory.flushBackingStore();
}

void Scheduler::dispatch() {
    for (size_t coreIndex = 0; coreIndex < cores.size(); ++coreIndex) {
        auto& core = cores[coreIndex];
        if (core.process) { continue; }
        while (!readyQueue.empty()) {
            auto process = readyQueue.front();
            readyQueue.pop_front();
            if (process->getState() != ProcessState::Ready) { continue; }
            core.process = process;
            core.quantumUsed = 0;
            process->setRunning(currentTick, static_cast<int>(coreIndex));
            break;
        }
    }
}

void Scheduler::run() {
    using clock = std::chrono::steady_clock;

    auto nextTick = clock::now();

    while (!shutdownRequested) {
        nextTick += std::chrono::milliseconds(100);

        tick();

        std::this_thread::sleep_until(nextTick);
    }
}
