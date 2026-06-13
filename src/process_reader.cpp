#include "process_reader.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cctype>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------- small helpers, each reads ONE thing from /proc ----------

// Current time in seconds (with decimals). Used to measure how much
// real time passed between two snapshots.
static double seconds_now() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

// /proc/[pid]/comm holds just the process name, e.g. "chrome"
static std::string read_name(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(file, name);
    return name;
}

// /proc/[pid]/stat field 14 (utime) + field 15 (stime) = total CPU time
// this process has ever used, measured in clock ticks.
static unsigned long long read_cpu_ticks(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(file, line);
    if (line.empty()) return 0;

    // The process name sits inside parentheses and can contain spaces,
    // which breaks naive splitting. Trick: parse from AFTER the last ')'.
    size_t after_name = line.rfind(')');
    if (after_name == std::string::npos) return 0;

    std::istringstream rest(line.substr(after_name + 2));
    std::vector<std::string> fields;
    std::string token;
    while (rest >> token) fields.push_back(token);

    // fields[0] is stat field 3 (state), so utime (field 14) lands at
    // index 11 and stime (field 15) at index 12.
    if (fields.size() < 13) return 0;
    return std::stoull(fields[11]) + std::stoull(fields[12]);
}

// /proc/[pid]/status has a line like "VmRSS:   123456 kB" — that's the
// physical RAM the process is using right now.
static double read_memory_mb(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            std::istringstream(line.substr(6)) >> kb;
            return kb / 1024.0;
        }
    }
    return 0.0;  // kernel threads have no VmRSS line at all
}

// /proc/[pid]/io gives lifetime disk read/write totals. Reading it for
// processes we don't own needs root, so on failure we just leave zeros.
static void read_disk_io(int pid, long long& read_bytes, long long& write_bytes) {
    read_bytes = 0;
    write_bytes = 0;
    std::ifstream file("/proc/" + std::to_string(pid) + "/io");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("read_bytes:", 0) == 0)
            read_bytes = std::stoll(line.substr(11));
        else if (line.rfind("write_bytes:", 0) == 0)
            write_bytes = std::stoll(line.substr(12));
    }
}

// ---------- the main snapshot function ----------

std::vector<ProcessInfo> ProcessReader::read_all() {
    std::vector<ProcessInfo> result;

    double now = seconds_now();
    double elapsed = now - prev_time_;
    long ticks_per_sec = sysconf(_SC_CLK_TCK);  // usually 100 on Linux

    // fresh maps for this snapshot — they replace the old ones at the end
    std::map<int, unsigned long long> cur_cpu;
    std::map<int, long long> cur_read, cur_write;

    for (const auto& entry : fs::directory_iterator("/proc")) {
        // process folders are the ones whose name is a number (the PID)
        std::string dirname = entry.path().filename().string();
        if (dirname.empty() || !std::isdigit(dirname[0])) continue;
        int pid = std::stoi(dirname);

        ProcessInfo info;
        info.pid = pid;
        info.name = read_name(pid);
        if (info.name.empty()) continue;  // process died while we were reading

        unsigned long long ticks = read_cpu_ticks(pid);
        long long rbytes = 0, wbytes = 0;
        read_disk_io(pid, rbytes, wbytes);
        info.memory_rss_mb = read_memory_mb(pid);

        // CPU% and disk I/O are "change since last sample" values, so we
        // can only compute them if we saw this PID in the previous snapshot.
        if (prev_time_ > 0 && elapsed > 0 && prev_cpu_ticks_.count(pid)) {
            double cpu_secs_used = (ticks - prev_cpu_ticks_[pid]) / (double)ticks_per_sec;
            info.cpu_percent = 100.0 * cpu_secs_used / elapsed;
            info.disk_read_bytes = rbytes - prev_read_bytes_[pid];
            info.disk_write_bytes = wbytes - prev_write_bytes_[pid];
        }

        cur_cpu[pid] = ticks;
        cur_read[pid] = rbytes;
        cur_write[pid] = wbytes;
        result.push_back(info);
    }

    // remember this snapshot so the next call can compute the deltas
    prev_cpu_ticks_ = std::move(cur_cpu);
    prev_read_bytes_ = std::move(cur_read);
    prev_write_bytes_ = std::move(cur_write);
    prev_time_ = now;

    return result;
}
