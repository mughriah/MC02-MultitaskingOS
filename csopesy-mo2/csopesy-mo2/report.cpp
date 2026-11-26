#include "report.h"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>

std::string Report::generate(Scheduler& scheduler) {
    const auto snapshot = scheduler.getSnapshot();
    const int utilization = snapshot.cpuCount > 0
        ? static_cast<int>(std::lround(100.0 * snapshot.usedCores / snapshot.cpuCount)) : 0;
    std::ostringstream out;
    out << "CPU utilization: " << utilization << "%\n"
        << "Cores used: " << snapshot.usedCores << '\n'
        << "Cores available: " << snapshot.cpuCount - snapshot.usedCores << "\n\n"
        << "--------------------------------------\n"
        << "Running processes:\n";
    for (const auto& p : snapshot.processes) {
        if (p.state == ProcessState::Finished || p.coreID < 0) { continue; }
        out << p.name << "   (" << p.creationTime << ")    Core: "
            << (p.coreID >= 0 ? std::to_string(p.coreID) : "-")
            << "     " << p.executed << " / " << p.total << '\n';
    }
    out << "\nReady / sleeping processes:\n";
    for (const auto& p : snapshot.processes) {
        if (p.state == ProcessState::Finished || p.coreID >= 0) { continue; }
        out << p.name << "   (" << p.creationTime << ")    " << processStateName(p.state)
            << "     " << p.executed << " / " << p.total << '\n';
    }
    out << "\nFinished processes:\n";
    for (const auto& p : snapshot.processes) {
        if (p.state != ProcessState::Finished) { continue; }
        out << p.name << "   (" << p.creationTime << ")    "
            << (p.violated ? "Terminated (memory access violation)" : "Finished") << "     "
            << p.executed << " / " << p.total << '\n';
    }
    out << "--------------------------------------\n";
    return out.str();
}

std::string Report::processSmi(Scheduler& scheduler) {
    const auto snapshot = scheduler.getSnapshot();
    const auto stats = scheduler.getMemoryStats();
    const auto resident = scheduler.getResidentBytes();

    const int cpuUtil = snapshot.cpuCount > 0
        ? static_cast<int>(std::lround(100.0 * snapshot.usedCores / snapshot.cpuCount)) : 0;
    const int memUtil = stats.totalBytes > 0
        ? static_cast<int>(std::lround(100.0 * stats.usedBytes / stats.totalBytes)) : 0;

    std::ostringstream out;
    out << "\n--------------------------------------------\n"
        << "| PROCESS-SMI V01.00 Driver Version: 01.00 |\n"
        << "--------------------------------------------\n"
        << "CPU-Util: " << cpuUtil << "%\n"
        << "Memory Usage: " << stats.usedBytes << "B / " << stats.totalBytes << "B\n"
        << "Memory Util: " << memUtil << "%\n\n"
        << "============================================\n"
        << "Running processes and memory usage:\n"
        << "--------------------------------------------\n";
    bool any = false;
    for (const auto& p : snapshot.processes) {
        if (p.state == ProcessState::Finished) { continue; }
        const auto it = resident.find(p.id);
        if (it == resident.end() || it->second == 0) { continue; }
        any = true;
        out << std::left << std::setw(12) << p.name << std::right
            << it->second << "B resident / " << p.memorySize << "B allocated\n";
    }
    if (!any) { out << "(no process currently occupies memory)\n"; }
    out << "--------------------------------------------\n\n";
    return out.str();
}

std::string Report::vmstat(Scheduler& scheduler) {
    const auto snapshot = scheduler.getSnapshot();
    const auto stats = scheduler.getMemoryStats();

    std::ostringstream out;
    auto line = [&out](uint64_t value, const std::string& label) {
        out << std::setw(14) << value << ' ' << label << '\n';
        };
    out << '\n';
    line(stats.totalBytes, "bytes total memory");
    line(stats.usedBytes, "bytes used memory");
    line(stats.freeBytes, "bytes free memory");
    line(stats.frameSize, "bytes per frame (page size)");
    line(stats.totalFrames, "frames total");
    line(stats.usedFrames, "frames used");
    line(stats.totalFrames - stats.usedFrames, "frames free");
    line(static_cast<uint64_t>(snapshot.activeProcesses), "active processes (running on a core)");
    line(static_cast<uint64_t>(snapshot.inactiveProcesses), "inactive processes (ready or sleeping)");
    line(static_cast<uint64_t>(snapshot.finishedProcesses), "finished processes");
    line(snapshot.idleCoreTicks, "idle cpu ticks");
    line(snapshot.busyCoreTicks, "active cpu ticks");
    line(snapshot.totalCoreTicks, "total cpu ticks");
    line(stats.pagedIn, "pages paged in");
    line(stats.pagedOut, "pages paged out");
    line(stats.pageFaults, "page faults");
    line(stats.swappedPages, "pages in backing store (csopesy-backing-store.txt)");
    out << '\n';
    return out.str();
}

bool Report::save(const std::string& report, const std::string& filename) {
    std::ofstream file(filename);
    if (!file) { return false; }
    file << report;
    file.close();
    return !file.fail();
}
