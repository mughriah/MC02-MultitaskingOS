#pragma once

#include "scheduler.h"

#include <string>

class Report {
public:
    // screen -ls / report-util
    static std::string generate(Scheduler& scheduler);

    // Main-menu "process-smi": summarized memory usage + processes holding memory.
    static std::string processSmi(Scheduler& scheduler);

    // Main-menu "vmstat": detailed memory, process and paging counters.
    static std::string vmstat(Scheduler& scheduler);

    static bool save(
        const std::string& report,
        const std::string& filename = "csopesy-log.txt"
    );
};
