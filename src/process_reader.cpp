#include "process_reader.h"

#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <cctype>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------- small helpers, each reads ONE thing from /proc ----------

// Returns the current time in seconds (with decimals).
// We use this to measure how many seconds passed between two snapshots,
// which is needed to turn raw CPU ticks into a percentage.
static double seconds_now() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double>(now).count();
}

// /proc/[pid]/comm contains just the process name, one word, e.g. "chrome".
// It's the simplest place to get the name — no parsing needed.
static std::string read_name(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(file, name);
    return name;
}

// /proc/[pid]/stat contains a lot of process info in one line.
// Fields 14 (utime) + 15 (stime) are the total CPU clock ticks this process
// has ever consumed. We read this twice and take the difference to get
// how much CPU it used *between* our two readings.
//
// Tricky part: field 2 is the process name wrapped in parentheses — e.g. "(my process)"
// and the name itself can contain spaces, which breaks simple whitespace splitting.
// Fix: find the LAST ')' in the line, then parse everything after it.
// Everything before that closing ')' is the name — we don't care about it here.
static unsigned long long read_cpu_ticks(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(file, line);
    if (line.empty()) return 0;

    // skip past the process name block "(name)" by finding the last ')'
    size_t after_name = line.rfind(')');
    if (after_name == std::string::npos) return 0;

    // parse the remaining fields after the closing ')'
    std::istringstream rest(line.substr(after_name + 2));
    std::vector<std::string> fields;
    std::string token;
    while (rest >> token) fields.push_back(token);

    // After the name: field 3 = state (index 0), then more fields.
    // utime is the 12th field after ')' (index 11), stime is 13th (index 12).
    if (fields.size() < 13) return 0;
    return std::stoull(fields[11]) + std::stoull(fields[12]);
}

// /proc/[pid]/status has many lines. We only care about "VmRSS" —
// Resident Set Size — which is the actual physical RAM the process is using.
// (VmSize would be virtual memory, which is usually much larger and less useful.)
static double read_memory_mb(int pid) {
    std::ifstream file("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            long kb = 0;
            std::istringstream(line.substr(6)) >> kb;
            return kb / 1024.0;  // convert KB -> MB
        }
    }
    return 0.0;  // kernel threads have no memory lines — that's normal
}

// /proc/[pid]/io contains lifetime totals for bytes read and written to disk.
// We read it twice and take the difference (same idea as CPU ticks).
// Reading another user's io file needs root — if it fails, we leave zeros.
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

// Walks every folder in /proc/, reads stats for each PID, and returns a list
// of ProcessInfo structs — one per running process.
//
// CPU% and disk I/O are "rate since last call" values, so:
//   - first call ever: those fields come back as 0 (no previous reading to diff against)
//   - every call after that: real values
// That's why the daemon does a warm-up read before starting its logging loop.
std::vector<ProcessInfo> ProcessReader::read_all() {
    std::vector<ProcessInfo> result;

    double now = seconds_now();
    double elapsed = now - prev_time_;          // seconds since last call
    long ticks_per_sec = sysconf(_SC_CLK_TCK); // usually 100 on Linux (100 ticks per second)

    // temporary maps to hold this snapshot's raw values
    std::map<int, unsigned long long> cur_cpu;
    std::map<int, long long> cur_read, cur_write;

    for (const auto& entry : fs::directory_iterator("/proc")) {
        // /proc/ has both numbered folders (PIDs) and named folders (like /proc/sys).
        // We only care about the numbered ones.
        std::string dirname = entry.path().filename().string();
        if (dirname.empty() || !std::isdigit(dirname[0])) continue;
        int pid = std::stoi(dirname);

        ProcessInfo info;
        info.pid  = pid;
        info.name = read_name(pid);
        if (info.name.empty()) continue;  // process exited between directory scan and name read

        // read raw values from /proc
        unsigned long long ticks = read_cpu_ticks(pid);
        long long rbytes = 0, wbytes = 0;
        read_disk_io(pid, rbytes, wbytes);
        info.memory_rss_mb = read_memory_mb(pid);

        // Compute CPU% and I/O rates only when we have a previous reading to diff against.
        // Formula: (tick_delta / ticks_per_second) / elapsed_seconds * 100
        // = fraction of one CPU core used in this interval, expressed as a percentage.
        if (prev_time_ > 0 && elapsed > 0 && prev_cpu_ticks_.count(pid)) {
            double cpu_secs_used = (ticks - prev_cpu_ticks_[pid]) / (double)ticks_per_sec;
            info.cpu_percent      = 100.0 * cpu_secs_used / elapsed;
            info.disk_read_bytes  = rbytes - prev_read_bytes_[pid];
            info.disk_write_bytes = wbytes - prev_write_bytes_[pid];
        }

        // save raw values for the next call to diff against
        cur_cpu[pid]   = ticks;
        cur_read[pid]  = rbytes;
        cur_write[pid] = wbytes;
        result.push_back(info);
    }

    // replace previous snapshot with current one
    prev_cpu_ticks_   = std::move(cur_cpu);
    prev_read_bytes_  = std::move(cur_read);
    prev_write_bytes_ = std::move(cur_write);
    prev_time_        = now;

    return result;
}
