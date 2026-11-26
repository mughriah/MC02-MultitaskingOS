#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class InstructionType {
    PRINT,
    DECLARE,
    ADD,
    SUBTRACT,
    SLEEP,
    FOR,
    READ,   // MO2: READ var address
    WRITE   // MO2: WRITE address value
};

// A PRINT message is a list of pieces: either literal text or a variable to expand.
struct PrintPart {
    bool isVariable = false;
    std::string text;
};

struct Instruction {
    InstructionType type = InstructionType::PRINT;

    std::vector<PrintPart> parts;     // PRINT
    std::string destination;          // DECLARE / ADD / SUBTRACT / READ
    std::string operand1;             // ADD / SUBTRACT / WRITE (value)
    std::string operand2;             // ADD / SUBTRACT

    uint16_t value = 0;               // DECLARE
    uint8_t sleepTicks = 0;           // SLEEP
    std::vector<Instruction> body;    // FOR
    uint64_t repeats = 0;             // FOR
    uint64_t address = 0;             // READ / WRITE (virtual address inside the process)

    static Instruction makePrint(const std::string& message);
    static Instruction makePrintParts(std::vector<PrintPart> parts);

    static Instruction makeDeclare(const std::string& variable, uint16_t value);

    static Instruction makeAdd(
        const std::string& destination,
        const std::string& operand1,
        const std::string& operand2
    );

    static Instruction makeSubtract(
        const std::string& destination,
        const std::string& operand1,
        const std::string& operand2
    );

    static Instruction makeSleep(uint8_t ticks);
    static Instruction makeFor(std::vector<Instruction> body, uint64_t repeats);

    // READ (var, memory_address): load a uint16 from memory into var.
    static Instruction makeRead(const std::string& variable, uint64_t address);
    // WRITE (memory_address, value): store a uint16 (literal or variable) to memory.
    static Instruction makeWrite(uint64_t address, const std::string& valueOperand);
};

// Loop headers do not consume CPU ticks; count the ordinary instructions they execute.
// Also validates the three-level nesting limit and rejects count overflow.
uint64_t countExecutableInstructions(const std::vector<Instruction>& program, unsigned depth = 0);

// Random program for scheduler-start / scheduler-test. READ/WRITE addresses always fall
// inside [0, memorySize - 2], so generated processes never cause access violations.
std::vector<Instruction> generateInstructions(
    const std::string& processName,
    uint64_t count,
    uint64_t memorySize
);

// ---- helpers used by the parser, the process and the console ----

// Decimal ("42") or hexadecimal ("0x2A") number. Very large values saturate.
bool parseNumberToken(const std::string& token, uint64_t& value);
// Hexadecimal address with an optional 0x prefix ("0x500", "500").
bool parseHexAddress(const std::string& token, uint64_t& value);
// "0x1F4"
std::string formatHexAddress(uint64_t address);

// Parses a semicolon-separated instruction string used by "screen -c".
// Supported: DECLARE v n | ADD d a b | SUBTRACT d a b | SLEEP n | READ v 0xADDR |
//            WRITE 0xADDR value | PRINT("text" + var) | FOR([ins; ins], n)
// Returns false and fills 'error' on any syntax problem. 'out' holds the top-level
// instructions (its size is the "number of instructions" limited to 1-50 by the console).
bool parseInstructions(const std::string& text, std::vector<Instruction>& out, std::string& error);
