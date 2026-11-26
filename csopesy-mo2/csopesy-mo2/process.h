#pragma once

#include "instruction.h"
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

class MemoryManager;

enum class ProcessState { Ready, Running, Sleeping, Finished };
enum class StepResult { Executed, BusyWait, Sleeping, Finished };
std::string processStateName(ProcessState state);

struct ProcessSnapshot {
    std::string name;
    int id = 0;
    ProcessState state = ProcessState::Ready;
    int coreID = -1;
    uint64_t executed = 0;
    uint64_t total = 0;
    uint64_t creationTick = 0;
    uint64_t finishTick = 0;
    std::string creationTime;
    std::string finishTime;
    std::vector<std::string> logs;

    // MO2
    uint64_t memorySize = 0;          // bytes allocated to the process
    bool violated = false;            // shut down by a memory access violation
    std::string violationTime;        // HH:MM:SS
    std::string violationAddress;     // e.g. 0x1F4
};

class Process {
private:
    struct ExecutionFrame {
        const std::vector<Instruction>* block;
        size_t index;
        uint64_t repetitionsLeft;
    };

    // The symbol table segment is 64 bytes = at most 32 uint16 variables.
    static constexpr int kMaxVariables = 32;
    static constexpr size_t kMaxLogs = 1000;

    mutable std::mutex mutex;
    std::string name;
    int processID;
    std::vector<Instruction> instructions;
    std::vector<ExecutionFrame> frames;
    std::map<std::string, int> slots;       // variable name -> symbol-table slot
    uint16_t values[kMaxVariables] = {};    // value held in each slot
    int variableCount = 0;
    std::deque<std::string> logs;
    ProcessState state = ProcessState::Ready;
    int coreID = -1;
    uint64_t memorySize = 0;
    uint64_t instructionCount = 0;
    uint64_t creationTick = 0;
    uint64_t startTick = 0;
    uint64_t finishTick = 0;
    uint64_t executedInstructions = 0;
    uint64_t sleepRemaining = 0;
    uint64_t delayRemaining = 0;
    bool violated = false;
    std::string violationTime;
    uint64_t violationAddress = 0;
    std::string creationTime;
    std::string finishTime;

    int findOrCreateSlot(const std::string& variable);
    uint16_t readVariable(const std::string& variable, MemoryManager& memory);
    void writeVariable(const std::string& variable, uint16_t value, MemoryManager& memory);
    uint16_t readOperand(const std::string& operand, MemoryManager& memory);
    bool addressInsideProcess(uint64_t address) const;
    StepResult raiseViolation(uint64_t address, uint64_t currentTick);
    const Instruction* nextInstruction();
    void finish(uint64_t currentTick);

public:
    Process(int id, const std::string& processName, std::vector<Instruction> program,
        uint64_t createdAt, uint64_t memoryBytes);
    StepResult executeOne(uint64_t currentTick, uint64_t delayPerExec, MemoryManager& memory);
    void advanceSleep(uint64_t currentTick);
    void setRunning(uint64_t currentTick, int cpu = 0);
    void setReady();
    ProcessSnapshot getSnapshot(bool includeLogs = false) const;
    std::string getName() const;
    int getID() const;
    ProcessState getState() const;
    uint64_t getMemorySize() const;
    bool isViolated() const;
    uint64_t getInstructionPointer() const;
    uint64_t getInstructionCount() const;
    uint64_t getCreationTick() const;
    uint64_t getStartTick() const;
    uint64_t getFinishTick() const;
    uint64_t getExecutedInstructions() const;
    uint64_t getSleepRemaining() const;
    std::vector<std::string> getLogs() const;
    std::string getStateString() const;
};
