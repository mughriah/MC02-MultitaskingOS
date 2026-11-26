#include "process.h"
#include "memory_manager.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace {
    std::string formatTime(const char* pattern) {
        const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        std::ostringstream text;
        text << std::put_time(&local, pattern);
        return text.str();
    }
    std::string timestamp() { return formatTime("%m/%d/%Y %I:%M:%S%p"); }
    std::string timeOfDay() { return formatTime("%H:%M:%S"); }
    uint16_t clamp16(uint64_t v) { return static_cast<uint16_t>(std::min<uint64_t>(v, 65535)); }
}

std::string processStateName(ProcessState state) {
    switch (state) {
    case ProcessState::Ready: return "Ready";
    case ProcessState::Running: return "Running";
    case ProcessState::Sleeping: return "Sleeping";
    case ProcessState::Finished: return "Finished";
    }
    return "Unknown";
}

Process::Process(int id, const std::string& processName, std::vector<Instruction> program,
    uint64_t createdAt, uint64_t memoryBytes)
    : name(processName), processID(id), instructions(std::move(program)),
    memorySize(memoryBytes),
    instructionCount(countExecutableInstructions(instructions)),
    creationTick(createdAt), creationTime(timestamp()) {
    frames.push_back({ &instructions, 0, 1 });
}

// Returns the symbol-table slot of a variable, declaring it if there is room.
// -1 means the 64-byte symbol table is full (32 variables): the access is ignored.
int Process::findOrCreateSlot(const std::string& variable) {
    const auto it = slots.find(variable);
    if (it != slots.end()) { return it->second; }
    if (variableCount >= kMaxVariables) { return -1; }
    const int slot = variableCount++;
    slots[variable] = slot;
    values[slot] = 0;
    return slot;
}

// Variables live in the symbol-table segment (virtual bytes 0..63), so touching one
// requires that segment's page to be resident - otherwise a page fault is serviced first.
uint16_t Process::readVariable(const std::string& variable, MemoryManager& memory) {
    const int slot = findOrCreateSlot(variable);
    if (slot < 0) { return 0; }
    memory.touchSymbol(processID, slot);
    return values[slot];
}

void Process::writeVariable(const std::string& variable, uint16_t value, MemoryManager& memory) {
    const int slot = findOrCreateSlot(variable);
    if (slot < 0) { return; }   // symbol table full: the declaration is ignored
    memory.touchSymbol(processID, slot);
    values[slot] = value;
}

uint16_t Process::readOperand(const std::string& operand, MemoryManager& memory) {
    if (operand.empty()) { return 0; }
    uint64_t literal = 0;
    if (parseNumberToken(operand, literal)) { return clamp16(literal); }
    return readVariable(operand, memory);
}

bool Process::addressInsideProcess(uint64_t address) const {
    // A uint16 occupies 2 bytes, so the last valid start address is memorySize - 2.
    return address < memorySize && memorySize - address >= 2;
}

const Instruction* Process::nextInstruction() {
    // The stack remembers each loop's position across quantum expiry and SLEEP.
    while (!frames.empty()) {
        auto& frame = frames.back();
        if (frame.index == frame.block->size()) {
            if (--frame.repetitionsLeft > 0) { frame.index = 0; }
            else { frames.pop_back(); }
            continue;
        }
        const Instruction* instruction = &(*frame.block)[frame.index++];
        if (instruction->type == InstructionType::FOR) {
            if (instruction->repeats && countExecutableInstructions(instruction->body) > 0) {
                frames.push_back({ &instruction->body, 0, instruction->repeats });
            }
            continue;
        }
        return instruction;
    }
    return nullptr;
}

void Process::finish(uint64_t currentTick) {
    state = ProcessState::Finished;
    coreID = -1;
    finishTick = currentTick;
    finishTime = timestamp();
    slots.clear();
    variableCount = 0;
    frames.clear();
    instructions.clear();
}

StepResult Process::raiseViolation(uint64_t address, uint64_t currentTick) {
    violated = true;
    violationAddress = address;
    violationTime = timeOfDay();
    finish(currentTick);
    return StepResult::Finished;
}

StepResult Process::executeOne(uint64_t currentTick, uint64_t delayPerExec, MemoryManager& memory) {
    std::lock_guard<std::mutex> lock(mutex);
    if (state == ProcessState::Finished) { return StepResult::Finished; }
    if (state == ProcessState::Sleeping) { return StepResult::Sleeping; }
    if (delayRemaining > 0) {
        --delayRemaining;
        return StepResult::BusyWait;
    }
    const Instruction* instruction = nextInstruction();
    if (!instruction) {
        finish(currentTick);
        return StepResult::Finished;
    }
    // Page faults are serviced inside readVariable/writeVariable/read16/write16 and repeat
    // until a valid frame holds the page, so the instruction only completes once its pages
    // are resident (the "restart the instruction" step of demand paging).
    switch (instruction->type) {
    case InstructionType::PRINT: {
        std::string text;
        for (const auto& part : instruction->parts) {
            if (part.isVariable) { text += std::to_string(readVariable(part.text, memory)); }
            else { text += part.text; }
        }
        logs.push_back("(" + timestamp() + ") Core:" + std::to_string(coreID) + " \"" + text + "\"");
        if (logs.size() > kMaxLogs) { logs.pop_front(); }
        break;
    }
    case InstructionType::DECLARE:
        writeVariable(instruction->destination, instruction->value, memory);
        break;
    case InstructionType::ADD: {
        const uint32_t left = readOperand(instruction->operand1, memory);
        const uint32_t right = readOperand(instruction->operand2, memory);
        writeVariable(instruction->destination,
            static_cast<uint16_t>(std::min<uint32_t>(left + right, 65535)), memory);
        break;
    }
    case InstructionType::SUBTRACT: {
        const uint32_t left = readOperand(instruction->operand1, memory);
        const uint32_t right = readOperand(instruction->operand2, memory);
        writeVariable(instruction->destination,
            static_cast<uint16_t>(left >= right ? left - right : 0), memory);
        break;
    }
    case InstructionType::SLEEP:
        sleepRemaining = instruction->sleepTicks;
        break;
    case InstructionType::READ: {
        const uint64_t address = instruction->address;
        if (!addressInsideProcess(address)) { return raiseViolation(address, currentTick); }
        // Uninitialized memory reads back as 0 (pages are zero-filled on first touch).
        const uint16_t loaded = memory.read16(processID, address);
        writeVariable(instruction->destination, loaded, memory);
        break;
    }
    case InstructionType::WRITE: {
        const uint64_t address = instruction->address;
        if (!addressInsideProcess(address)) { return raiseViolation(address, currentTick); }
        const uint16_t stored = readOperand(instruction->operand1, memory);
        memory.write16(processID, address, stored);
        break;
    }
    case InstructionType::FOR:
        break; // Loop headers are handled by nextInstruction().
    }
    ++executedInstructions;
    delayRemaining = delayPerExec;
    if (sleepRemaining > 0) {
        state = ProcessState::Sleeping;
        coreID = -1;
        return StepResult::Sleeping;
    }
    if (executedInstructions == instructionCount) {
        finish(currentTick);
        return StepResult::Finished;
    }
    return StepResult::Executed;
}

void Process::advanceSleep(uint64_t currentTick) {
    std::lock_guard<std::mutex> lock(mutex);
    if (state != ProcessState::Sleeping) { return; }
    if (sleepRemaining > 0) { --sleepRemaining; }
    if (sleepRemaining == 0) {
        if (executedInstructions == instructionCount) { finish(currentTick); }
        else { state = ProcessState::Ready; }
    }
}

void Process::setRunning(uint64_t currentTick, int cpu) {
    std::lock_guard<std::mutex> lock(mutex);
    if (state == ProcessState::Finished || state == ProcessState::Sleeping) { return; }
    state = ProcessState::Running;
    coreID = cpu;
    if (startTick == 0) { startTick = currentTick; }
}

void Process::setReady() {
    std::lock_guard<std::mutex> lock(mutex);
    if (state == ProcessState::Running) {
        state = ProcessState::Ready;
        coreID = -1;
    }
}

ProcessSnapshot Process::getSnapshot(bool includeLogs) const {
    std::lock_guard<std::mutex> lock(mutex);
    ProcessSnapshot snapshot;
    snapshot.name = name;
    snapshot.id = processID;
    snapshot.state = state;
    snapshot.coreID = coreID;
    snapshot.executed = executedInstructions;
    snapshot.total = instructionCount;
    snapshot.creationTick = creationTick;
    snapshot.finishTick = finishTick;
    snapshot.creationTime = creationTime;
    snapshot.finishTime = finishTime;
    snapshot.memorySize = memorySize;
    snapshot.violated = violated;
    snapshot.violationTime = violationTime;
    snapshot.violationAddress = violated ? formatHexAddress(violationAddress) : std::string();
    if (includeLogs) { snapshot.logs.assign(logs.begin(), logs.end()); }
    return snapshot;
}

std::string Process::getName() const { std::lock_guard<std::mutex> lock(mutex); return name; }
int Process::getID() const { std::lock_guard<std::mutex> lock(mutex); return processID; }
ProcessState Process::getState() const { std::lock_guard<std::mutex> lock(mutex); return state; }
uint64_t Process::getMemorySize() const { std::lock_guard<std::mutex> lock(mutex); return memorySize; }
bool Process::isViolated() const { std::lock_guard<std::mutex> lock(mutex); return violated; }
uint64_t Process::getInstructionPointer() const { std::lock_guard<std::mutex> lock(mutex); return executedInstructions; }
uint64_t Process::getInstructionCount() const { std::lock_guard<std::mutex> lock(mutex); return instructionCount; }
uint64_t Process::getCreationTick() const { std::lock_guard<std::mutex> lock(mutex); return creationTick; }
uint64_t Process::getStartTick() const { std::lock_guard<std::mutex> lock(mutex); return startTick; }
uint64_t Process::getFinishTick() const { std::lock_guard<std::mutex> lock(mutex); return finishTick; }
uint64_t Process::getExecutedInstructions() const { std::lock_guard<std::mutex> lock(mutex); return executedInstructions; }
uint64_t Process::getSleepRemaining() const { std::lock_guard<std::mutex> lock(mutex); return sleepRemaining; }

std::vector<std::string> Process::getLogs() const {
    std::lock_guard<std::mutex> lock(mutex);
    return { logs.begin(), logs.end() };
}

std::string Process::getStateString() const { std::lock_guard<std::mutex> lock(mutex); return processStateName(state); }
