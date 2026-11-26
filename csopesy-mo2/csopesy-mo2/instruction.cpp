#include "instruction.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

// ------------------------------------------------------------------ factories

Instruction Instruction::makePrint(const std::string& msg) {
    Instruction instruction;
    instruction.type = InstructionType::PRINT;
    instruction.parts.push_back({ false, msg });
    return instruction;
}

Instruction Instruction::makePrintParts(std::vector<PrintPart> parts) {
    Instruction instruction;
    instruction.type = InstructionType::PRINT;
    instruction.parts = std::move(parts);
    return instruction;
}

Instruction Instruction::makeDeclare(const std::string& variable, uint16_t initialValue) {
    Instruction instruction;
    instruction.type = InstructionType::DECLARE;
    instruction.destination = variable;
    instruction.value = initialValue;
    return instruction;
}

Instruction Instruction::makeAdd(
    const std::string& destination,
    const std::string& operand1,
    const std::string& operand2
) {
    Instruction instruction;
    instruction.type = InstructionType::ADD;
    instruction.destination = destination;
    instruction.operand1 = operand1;
    instruction.operand2 = operand2;
    return instruction;
}

Instruction Instruction::makeSubtract(
    const std::string& destination,
    const std::string& operand1,
    const std::string& operand2
) {
    Instruction instruction;
    instruction.type = InstructionType::SUBTRACT;
    instruction.destination = destination;
    instruction.operand1 = operand1;
    instruction.operand2 = operand2;
    return instruction;
}

Instruction Instruction::makeSleep(uint8_t ticks) {
    Instruction instruction;
    instruction.type = InstructionType::SLEEP;
    instruction.sleepTicks = ticks;
    return instruction;
}

Instruction Instruction::makeFor(std::vector<Instruction> body, uint64_t repeats) {
    Instruction instruction;
    instruction.type = InstructionType::FOR;
    instruction.body = std::move(body);
    instruction.repeats = repeats;
    return instruction;
}

Instruction Instruction::makeRead(const std::string& variable, uint64_t address) {
    Instruction instruction;
    instruction.type = InstructionType::READ;
    instruction.destination = variable;
    instruction.address = address;
    return instruction;
}

Instruction Instruction::makeWrite(uint64_t address, const std::string& valueOperand) {
    Instruction instruction;
    instruction.type = InstructionType::WRITE;
    instruction.address = address;
    instruction.operand1 = valueOperand;
    return instruction;
}

uint64_t countExecutableInstructions(const std::vector<Instruction>& program, unsigned depth) {
    uint64_t total = 0;
    for (const auto& instruction : program) {
        uint64_t count = 1;
        if (instruction.type == InstructionType::FOR) {
            if (depth >= 3) { throw std::invalid_argument("FOR loops may nest at most three levels."); }
            const uint64_t bodyCount = countExecutableInstructions(instruction.body, depth + 1);
            if (instruction.repeats && bodyCount > std::numeric_limits<uint64_t>::max() / instruction.repeats) {
                throw std::overflow_error("FOR instruction count overflow.");
            }
            count = bodyCount * instruction.repeats;
        }
        if (count > std::numeric_limits<uint64_t>::max() - total) {
            throw std::overflow_error("Program instruction count overflow.");
        }
        total += count;
    }
    return total;
}

// ------------------------------------------------------------------ helpers

bool parseNumberToken(const std::string& token, uint64_t& value) {
    constexpr uint64_t cap = uint64_t{ 1 } << 48;
    if (token.empty()) { return false; }
    value = 0;
    if (token.size() > 2 && token[0] == '0' && (token[1] == 'x' || token[1] == 'X')) {
        for (size_t i = 2; i < token.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(token[i]);
            if (!std::isxdigit(c)) { return false; }
            const uint64_t digit = std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10;
            if (value < cap) { value = value * 16 + digit; }
        }
        return true;
    }
    for (unsigned char c : token) {
        if (!std::isdigit(c)) { return false; }
        if (value < cap) { value = value * 10 + (c - '0'); }
    }
    return true;
}

bool parseHexAddress(const std::string& token, uint64_t& value) {
    std::string digits = token;
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits = digits.substr(2);
    }
    if (digits.empty() || digits.size() > 12) { return false; }
    value = 0;
    for (unsigned char c : digits) {
        if (!std::isxdigit(c)) { return false; }
        const uint64_t digit = std::isdigit(c) ? c - '0' : std::tolower(c) - 'a' + 10;
        value = value * 16 + digit;
    }
    return true;
}

std::string formatHexAddress(uint64_t address) {
    std::ostringstream text;
    text << "0x" << std::uppercase << std::hex << address;
    return text.str();
}

// ------------------------------------------------------------------ parser

namespace {
    std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) { ++a; }
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { --b; }
        return s.substr(a, b - a);
    }

    std::string upper(std::string s) {
        for (auto& c : s) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
        return s;
    }

    bool isIdentifier(const std::string& s) {
        if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) { return false; }
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    }

    bool isOperand(const std::string& s) {
        uint64_t ignored;
        return isIdentifier(s) || parseNumberToken(s, ignored);
    }

    uint16_t clamp16(uint64_t v) { return static_cast<uint16_t>(std::min<uint64_t>(v, 65535)); }

    // Split on ';' that is outside quotes and outside (...) / [...] nesting.
    std::vector<std::string> splitStatements(const std::string& text) {
        std::vector<std::string> parts;
        std::string current;
        bool inQuote = false;
        int depth = 0;
        for (char c : text) {
            if (c == '"') { inQuote = !inQuote; }
            else if (!inQuote) {
                if (c == '(' || c == '[') { ++depth; }
                else if ((c == ')' || c == ']') && depth > 0) { --depth; }
            }
            if (c == ';' && !inQuote && depth == 0) {
                parts.push_back(current);
                current.clear();
            }
            else { current += c; }
        }
        parts.push_back(current);
        std::vector<std::string> result;
        for (const auto& p : parts) {
            const std::string t = trim(p);
            if (!t.empty()) { result.push_back(t); }
        }
        return result;
    }

    bool parseList(const std::string& text, unsigned depth, std::vector<Instruction>& out, std::string& error);

    bool parseStatement(const std::string& statement, unsigned depth, Instruction& out, std::string& error) {
        size_t i = 0;
        while (i < statement.size() && std::isalpha(static_cast<unsigned char>(statement[i]))) { ++i; }
        const std::string op = upper(statement.substr(0, i));
        const std::string rest = trim(statement.substr(i));

        if (op == "PRINT") {
            if (rest.size() < 2 || rest.front() != '(' || rest.back() != ')') {
                error = "PRINT needs the form PRINT(\"text\" + variable): " + statement;
                return false;
            }
            const std::string inner = trim(rest.substr(1, rest.size() - 2));
            std::vector<std::string> tokens;
            std::string current;
            bool inQuote = false;
            for (char c : inner) {
                if (c == '"') { inQuote = !inQuote; }
                if (c == '+' && !inQuote) { tokens.push_back(current); current.clear(); }
                else { current += c; }
            }
            tokens.push_back(current);
            if (inQuote) { error = "Unterminated string in: " + statement; return false; }
            std::vector<PrintPart> parts;
            for (const auto& raw : tokens) {
                const std::string token = trim(raw);
                if (token.size() >= 2 && token.front() == '"' && token.back() == '"') {
                    parts.push_back({ false, token.substr(1, token.size() - 2) });
                }
                else if (isIdentifier(token)) { parts.push_back({ true, token }); }
                else { error = "Bad PRINT argument '" + token + "'."; return false; }
            }
            out = Instruction::makePrintParts(std::move(parts));
            return true;
        }

        if (op == "FOR") {
            if (depth >= 3) { error = "FOR loops may nest at most three levels."; return false; }
            if (rest.size() < 2 || rest.front() != '(' || rest.back() != ')') {
                error = "FOR needs the form FOR([instructions], repeats): " + statement;
                return false;
            }
            const std::string inner = trim(rest.substr(1, rest.size() - 2));
            if (inner.empty() || inner[0] != '[') { error = "FOR body must be inside [ ]."; return false; }
            size_t close = std::string::npos;
            int nesting = 0;
            bool inQuote = false;
            for (size_t k = 0; k < inner.size(); ++k) {
                const char c = inner[k];
                if (c == '"') { inQuote = !inQuote; }
                if (inQuote) { continue; }
                if (c == '[') { ++nesting; }
                else if (c == ']' && --nesting == 0) { close = k; break; }
            }
            if (close == std::string::npos) { error = "FOR body is missing a closing ]."; return false; }
            const std::string bodyText = inner.substr(1, close - 1);
            std::string after = trim(inner.substr(close + 1));
            if (after.empty() || after[0] != ',') { error = "FOR needs a repeat count after the body."; return false; }
            uint64_t repeats = 0;
            if (!parseNumberToken(trim(after.substr(1)), repeats) || repeats == 0) {
                error = "FOR repeat count must be a positive number.";
                return false;
            }
            std::vector<Instruction> body;
            if (!parseList(bodyText, depth + 1, body, error)) { return false; }
            if (body.empty()) { error = "FOR body is empty."; return false; }
            out = Instruction::makeFor(std::move(body), repeats);
            return true;
        }

        std::istringstream args(rest);
        std::vector<std::string> a;
        for (std::string token; args >> token;) { a.push_back(token); }

        auto needArgs = [&](size_t n) {
            if (a.size() != n) {
                error = op + " expects " + std::to_string(n) + " argument(s): " + statement;
                return false;
            }
            return true;
            };

        if (op == "DECLARE") {
            uint64_t value = 0;
            if (!needArgs(2)) { return false; }
            if (!isIdentifier(a[0]) || !parseNumberToken(a[1], value)) {
                error = "DECLARE needs a variable name and a number: " + statement;
                return false;
            }
            out = Instruction::makeDeclare(a[0], clamp16(value));
            return true;
        }
        if (op == "ADD" || op == "SUBTRACT" || op == "SUB") {
            if (!needArgs(3)) { return false; }
            if (!isIdentifier(a[0]) || !isOperand(a[1]) || !isOperand(a[2])) {
                error = op + " needs a destination variable and two operands: " + statement;
                return false;
            }
            out = op == "ADD" ? Instruction::makeAdd(a[0], a[1], a[2])
                : Instruction::makeSubtract(a[0], a[1], a[2]);
            return true;
        }
        if (op == "SLEEP") {
            uint64_t ticks = 0;
            if (!needArgs(1)) { return false; }
            if (!parseNumberToken(a[0], ticks)) { error = "SLEEP needs a number: " + statement; return false; }
            out = Instruction::makeSleep(static_cast<uint8_t>(std::min<uint64_t>(ticks, 255)));
            return true;
        }
        if (op == "READ") {
            uint64_t address = 0;
            if (!needArgs(2)) { return false; }
            if (!isIdentifier(a[0]) || !parseHexAddress(a[1], address)) {
                error = "READ needs a variable and a hex address (READ var 0x1000): " + statement;
                return false;
            }
            out = Instruction::makeRead(a[0], address);
            return true;
        }
        if (op == "WRITE") {
            uint64_t address = 0;
            if (!needArgs(2)) { return false; }
            if (!parseHexAddress(a[0], address) || !isOperand(a[1])) {
                error = "WRITE needs a hex address and a value (WRITE 0x2000 42): " + statement;
                return false;
            }
            out = Instruction::makeWrite(address, a[1]);
            return true;
        }
        error = "Unknown instruction '" + (op.empty() ? statement : op) + "'.";
        return false;
    }

    bool parseList(const std::string& text, unsigned depth, std::vector<Instruction>& out, std::string& error) {
        for (const auto& statement : splitStatements(text)) {
            Instruction instruction;
            if (!parseStatement(statement, depth, instruction, error)) { return false; }
            out.push_back(std::move(instruction));
        }
        return true;
    }
}

bool parseInstructions(const std::string& text, std::vector<Instruction>& out, std::string& error) {
    out.clear();
    error.clear();
    if (!parseList(text, 0, out, error)) { out.clear(); return false; }
    return true;
}

// ------------------------------------------------------------------ random generation

namespace {
    std::vector<Instruction> generateBlock(
        const std::string& processName,
        uint64_t count,
        unsigned depth,
        uint64_t memorySize
    ) {
        std::vector<Instruction> instructions;

        static thread_local std::mt19937 rng(std::random_device{}());

        std::uniform_int_distribution<int> operation(0, 7);
        std::uniform_int_distribution<int> number(0, 100);
        std::uniform_int_distribution<int> sleepLength(1, 5);
        // Even addresses in [0, memorySize - 2] so the 2-byte access always fits.
        const uint64_t slots = memorySize >= 2 ? (memorySize - 2) / 2 + 1 : 1;
        std::uniform_int_distribution<uint64_t> addressSlot(0, slots - 1);

        const std::string variable = "var";

        uint64_t remaining = count;
        while (remaining > 0) {
            int choice = operation(rng);
            // A large workload stays compact instead of allocating billions of instructions.
            if (remaining > 4096 && depth < 3) { choice = 5; }
            if (choice == 5 && remaining >= 2 && depth < 3) {
                const uint64_t bodyCount = std::uniform_int_distribution<uint64_t>(1, std::min<uint64_t>(8, remaining / 2))(rng);
                const uint64_t repeats = remaining > 4096
                    ? remaining / bodyCount
                    : std::uniform_int_distribution<uint64_t>(2, std::min<uint64_t>(4, remaining / bodyCount))(rng);
                instructions.push_back(Instruction::makeFor(generateBlock(processName, bodyCount, depth + 1, memorySize), repeats));
                remaining -= bodyCount * repeats;
                continue;
            }
            if (choice == 5) { choice = 0; }
            switch (choice) {
            case 0:
                instructions.push_back(Instruction::makePrint("Hello world from " + processName + "!"));
                break;
            case 1:
                instructions.push_back(Instruction::makeDeclare(variable, static_cast<uint16_t>(number(rng))));
                break;
            case 2:
                instructions.push_back(Instruction::makeAdd(variable, variable, std::to_string(number(rng))));
                break;
            case 3:
                instructions.push_back(Instruction::makeSubtract(variable, variable, std::to_string(number(rng))));
                break;
            case 4:
                instructions.push_back(Instruction::makeSleep(static_cast<uint8_t>(sleepLength(rng))));
                break;
            case 6:
                instructions.push_back(Instruction::makeRead(variable, addressSlot(rng) * 2));
                break;
            case 7:
                instructions.push_back(Instruction::makeWrite(
                    addressSlot(rng) * 2,
                    number(rng) % 2 == 0 ? variable : std::to_string(number(rng))));
                break;
            }
            --remaining;
        }

        return instructions;
    }
}

std::vector<Instruction> generateInstructions(
    const std::string& processName,
    uint64_t count,
    uint64_t memorySize
) {
    return generateBlock(processName, count, 0, memorySize);
}
