
#include "console.h"
#include "report.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {
    // Memory size for screen -s / screen -c: digits only, power of 2 in [64, 65536].
    bool parseMemorySize(const std::string& token, uint64_t& size) {
        if (token.empty() || token.size() > 9 ||
            !std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            return false;
        }
        size = std::stoull(token);
        return isValidMemorySize(size);
    }
}

void Console::printWelcome() const {
    std::cout
        << "============================================================\n"
        << "  Group 8 | CSOPESY - MO2 Multitasking OS\n"
        << "============================================================\n"
        << "  Developers:\n"
        << "    Abenojar, Fred\n"
        << "    Caya, Mary Faye\n"
        << "    Diamante, Deo Zamir\n"
        << "    Guiller, Gerylyn\n"
        << "  Version Date: November 26, 2026\n"
        << "============================================================\n";
    if (initialized) {
        std::cout << "Type help for commands, or exit to quit.\n\n";
    }
    else {
        std::cout << "Type initialize first (or exit).\n"
            << "After initialization, type help for commands.\n\n";
    }
}

void Console::printHelp() const {
    std::cout
        << "\nAvailable commands:\n"
        << "  initialize\n"
        << "  screen -s <process_name> <process_memory_size>\n"
        << "  screen -c <process_name> <process_memory_size> \"<instructions>\"\n"
        << "  screen -r <process_name>\n"
        << "  screen -ls\n"
        << "  scheduler-start   (alias: scheduler-test)\n"
        << "  scheduler-stop\n"
        << "  report-util\n"
        << "  process-smi\n"
        << "  vmstat\n"
        << "  help\n"
        << "  exit\n\n"
        << "Memory sizes are powers of 2 between 64 and 65536 bytes.\n"
        << "Instructions (1-50, separated by ';'): DECLARE, ADD, SUBTRACT, SLEEP,\n"
        << "  READ var 0xADDR, WRITE 0xADDR value, PRINT(\"text\" + var), FOR([...], n)\n\n";
}

void Console::clearConsole() const {
    // Finish pending output before changing the display or moving its cursor.
    std::cout.flush();
#ifdef _WIN32
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD originalMode = 0;
    // Redirected output is a transcript, not a screen to erase.
    if (!GetConsoleMode(console, &originalMode)) { return; }

    // Windows Terminal/ConPTY handles VT clearing consistently. Filling the
    // entire legacy scrollback buffer can leave stale or duplicated rows.
    if (SetConsoleMode(console, originalMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        constexpr char sequence[] = "\x1b[2J\x1b[H";
        constexpr DWORD length = sizeof(sequence) - 1;
        DWORD written = 0;
        const BOOL cleared = WriteConsoleA(console, sequence, length, &written, nullptr);
        SetConsoleMode(console, originalMode);
        if (cleared && written == length) { return; }
    }

    // Fallback for older consoles: clear only the visible rows, then home
    // within that window instead of jumping to the start of scrollback.
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(console, &info)) { return; }
    const COORD origin = { 0, info.srWindow.Top };
    const DWORD cells = static_cast<DWORD>(info.dwSize.X)
        * static_cast<DWORD>(info.srWindow.Bottom - info.srWindow.Top + 1);
    DWORD written = 0;
    FillConsoleOutputCharacterA(console, ' ', cells, origin, &written);
    FillConsoleOutputAttribute(console, info.wAttributes, cells, origin, &written);
    SetConsoleCursorPosition(console, origin);
#else
    std::cout << "\x1b[2J\x1b[H" << std::flush;
#endif
}

std::string Console::readLine(
    const std::string& prompt
) const {
    std::cout << prompt;

    std::string line;
    std::getline(std::cin, line);

    return line;
}

void Console::run() {
    clearConsole();
    printWelcome();

    while (running) {
        std::string prompt = initialized
            ? "CSOPESY> "
            : "CSOPESY (not initialized)> ";

        std::string commandLine = readLine(prompt);

        if (!std::cin) {
            break;
        }

        executeCommand(commandLine);
    }

    if (initialized) {
        scheduler.stop();
    }

    std::cout << "CSOPESY terminated.\n";
}

void Console::executeCommand(
    const std::string& commandLine
) {
    std::istringstream stream(commandLine);
    std::string command;

    stream >> command;

    if (command.empty()) {
        return;
    }

    if (command == "exit") {
        running = false;
        return;
    }

    if (command == "initialize") {
        if (initialized) {
            std::cout
                << "System is already initialized.\n";
            return;
        }

        std::string configError;
        if (!config.loadFromFile("config.txt", &configError)) {
            std::cout << "Failed to load config.txt: " << configError << '\n';
            return;
        }

        if (!scheduler.initialize(config)) {
            std::cout
                << "Failed to initialize scheduler.\n";
            return;
        }

        scheduler.start();
        initialized = true;

        std::cout
            << "Configuration loaded successfully.\n";

        std::cout
            << "CPUs: " << config.numCPU
            << ", scheduler: " << config.scheduler
            << '\n'
            << "Memory: " << config.maxOverallMem << " bytes in "
            << config.maxOverallMem / config.memPerFrame << " frames of "
            << config.memPerFrame << " bytes\n";

        return;
    }

    if (!initialized) {
        std::cout
            << "Please run initialize before using other commands.\n";
        return;
    }

    if (command == "help") {
        printHelp();
    }
    else if (command == "screen") {
        handleScreenCommand(commandLine);
    }
    else if (command == "scheduler-start" || command == "scheduler-test") {
        scheduler.startBatchGeneration();

        std::cout
            << "Scheduler batch generation started.\n";
    }
    else if (command == "scheduler-stop") {
        scheduler.stopBatchGeneration();

        std::cout
            << "Scheduler batch generation stopped.\n";
    }
    else if (command == "report-util") {
        const std::string report = Report::generate(scheduler);
        std::cout << report;

        if (Report::save(report)) {
            std::cout
                << "Report saved to csopesy-log.txt\n";
        }
        else {
            std::cout
                << "Unable to write csopesy-log.txt\n";
        }
    }
    else if (command == "process-smi") {
        std::cout << Report::processSmi(scheduler);
    }
    else if (command == "vmstat") {
        std::cout << Report::vmstat(scheduler);
    }
    else {
        std::cout
            << "Unknown command: "
            << command
            << '\n';
    }
}

void Console::printViolation(const ProcessSnapshot& process) const {
    std::cout << "Process " << process.name
        << " shut down due to memory access violation error that occurred at "
        << process.violationTime << ". "
        << process.violationAddress << " invalid.\n";
}

void Console::handleScreenCommand(
    const std::string& commandLine
) {
    std::istringstream stream(commandLine);

    std::string command;
    std::string option;

    stream >> command >> option;

    if (option == "-ls") {
        std::cout << Report::generate(scheduler);
        return;
    }

    // screen -s <process_name> <process_memory_size>
    if (option == "-s") {
        std::string processName, sizeToken, extra;
        stream >> processName >> sizeToken;

        if (processName.empty() || sizeToken.empty() || (stream >> extra)) {
            std::cout << "Usage: screen -s <process_name> <process_memory_size>\n";
            return;
        }

        uint64_t memorySize = 0;
        if (!parseMemorySize(sizeToken, memorySize)) {
            std::cout << "invalid memory allocation\n";
            return;
        }

        auto process = scheduler.createProcess(processName, memorySize);
        if (!process) {
            std::cout << "Process name already exists or is invalid.\n";
            return;
        }

        enterProcessScreen(processName, true);
        return;
    }

    // screen -c <process_name> <process_memory_size> "<instructions>"
    if (option == "-c") {
        // The instruction string sits between the first and the last double quote. Quotes
        // inside it may be typed plainly or escaped as \" - both forms work.
        const size_t open = commandLine.find('"');
        const size_t close = commandLine.rfind('"');
        if (open == std::string::npos || close == open) {
            std::cout << "invalid command\n";
            return;
        }

        std::istringstream head(commandLine.substr(0, open));
        std::string processName, sizeToken, extra;
        head >> command >> option >> processName >> sizeToken;
        if (processName.empty() || sizeToken.empty() || (head >> extra)) {
            std::cout << "invalid command\n"
                << "Usage: screen -c <process_name> <process_memory_size> \"<instructions>\"\n";
            return;
        }

        uint64_t memorySize = 0;
        if (!parseMemorySize(sizeToken, memorySize)) {
            std::cout << "invalid memory allocation\n";
            return;
        }

        std::string text = commandLine.substr(open + 1, close - open - 1);
        for (size_t at = text.find("\\\""); at != std::string::npos; at = text.find("\\\"", at + 1)) {
            text.replace(at, 2, "\"");
        }

        std::vector<Instruction> program;
        std::string parseError;
        if (!parseInstructions(text, program, parseError)) {
            std::cout << "invalid command\n" << parseError << '\n';
            return;
        }
        if (program.empty() || program.size() > 50) {
            std::cout << "invalid command\n"
                << "A process needs between 1 and 50 instructions (got "
                << program.size() << ").\n";
            return;
        }

        std::shared_ptr<Process> process;
        try {
            process = scheduler.createProcess(processName, memorySize, std::move(program));
        }
        catch (const std::exception& error) {
            std::cout << "invalid command\n" << error.what() << '\n';
            return;
        }
        if (!process) {
            std::cout << "Process name already exists or is invalid.\n";
            return;
        }

        enterProcessScreen(processName, true);
        return;
    }

    if (option == "-r") {
        std::string processName;
        stream >> processName;

        if (processName.empty()) {
            std::cout
                << "Please provide a process name.\n";
            return;
        }

        auto process = scheduler.findProcess(processName);
        if (!process) {
            std::cout << "Process " << processName << " not found.\n";
            return;
        }

        const auto snapshot = process->getSnapshot();
        // MO2: a process killed by a memory access violation reports why and when.
        if (snapshot.violated) {
            printViolation(snapshot);
            return;
        }
        if (snapshot.state == ProcessState::Finished) {
            std::cout
                << "Process "
                << processName
                << " not found.\n";
            return;
        }

        enterProcessScreen(processName);
        return;
    }

    std::cout
        << "Usage: screen -s <name> <memory>, screen -c <name> <memory> \"<instructions>\", "
        << "screen -r <name>, or screen -ls\n";
}

void Console::printProcessInfo(const ProcessSnapshot& process) const {
    std::cout << "\nProcess name: " << process.name << '\n'
        << "ID: " << process.id << '\n'
        << "Memory: " << process.memorySize << " bytes\n"
        << "Logs:\n";
    for (const auto& log : process.logs) { std::cout << log << '\n'; }
    std::cout << '\n';
    if (process.violated) {
        printViolation(process);
        std::cout << '\n';
    }
    else if (process.state == ProcessState::Finished) {
        std::cout << "Finished!\n\n";
    }
    else {
        std::cout << "Current instruction line: " << process.executed << '\n'
            << "Lines of code: " << process.total << "\n\n";
    }
}

void Console::enterProcessScreen(const std::string& processName, bool newlyCreated) {
    const auto process = scheduler.findProcess(processName);
    if (!process) {
        std::cout << "Process " << processName << " not found.\n";
        return;
    }
    const auto snapshot = process->getSnapshot(true);
    if (!newlyCreated && snapshot.state == ProcessState::Finished) {
        std::cout << "Process " << processName << " not found.\n";
        return;
    }
    clearConsole();
    std::cout << "============================================\n"
        << "Process Screen: " << processName << '\n'
        << "Type process-smi for updated information.\n"
        << "Type exit to return to the main menu.\n"
        << "============================================\n";
    printProcessInfo(snapshot);
    while (true) {
        const std::string line = readLine(processName + "> ");
        if (!std::cin) { break; }
        std::istringstream stream(line);
        std::string command, extra;
        if (!(stream >> command)) { continue; }
        if (stream >> extra) {
            std::cout << "Use process-smi or exit without extra arguments.\n";
        }
        else if (command == "exit") { break; }
        else if (command == "process-smi") { printProcessInfo(process->getSnapshot(true)); }
        else { std::cout << "Unknown process command.\n"; }
    }
    clearConsole();
    printWelcome();
}
