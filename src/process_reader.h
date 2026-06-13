#ifndef PROCESS_READER_H
#define PROCESS_READER_H

#include <string>
#include <vector>
#include <map>

// Everything we know about one process at one moment in time.
struct ProcessInfo {
    int pid = 0;
    std::string name;
    double cpu_percent = 0.0;        // % of one CPU core used since last sample
    double memory_rss_mb = 0.0;      // physical RAM being used, in MB
    long long disk_read_bytes = 0;   // bytes read from disk since last sample
    long long disk_write_bytes = 0;  // bytes written to disk since last sample
};

class ProcessReader {
public:
    // Scans /proc and returns a snapshot of every running process.
    // CPU% and disk I/O are rates (change since last call), so the
    // very first call reports 0 for those — there's no "before" yet.
    std::vector<ProcessInfo> read_all();

private:
    // We remember the previous reading per PID so the next call can
    // answer "how much CPU/disk did this process use since last time?"
    std::map<int, unsigned long long> prev_cpu_ticks_;
    std::map<int, long long> prev_read_bytes_;
    std::map<int, long long> prev_write_bytes_;
    double prev_time_ = 0.0;  // when the last snapshot was taken (seconds)
};

#endif
