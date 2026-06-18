// endpoint-monitor
//
// A daemon that watches every process on the machine and logs its
// CPU / memory / disk / network activity as JSON every few seconds.
// A Python sidecar (ml/anomaly_detector.py) reads that log and flags anomalies.
//
// Usage:
//   ./monitor                          run the daemon (5s interval, default)
//   ./monitor --interval 10            custom sampling interval in seconds
//   ./monitor --output logs/m.json     custom log file path
//   ./monitor --status                 one-shot table of all processes, sorted by CPU
//   ./monitor --process chrome         show only processes matching a name
//   ./monitor --alerts                 print the last 10 anomaly alerts

#include "process_reader.h"
#include "network_tracker.h"
#include "json_writer.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <deque>
#include <string>
#include <algorithm>
#include <thread>
#include <chrono>

// Takes two process snapshots exactly 1 second apart and returns the second one.
// We need two reads because CPU% is a rate — you can't know how fast something
// is going without measuring it at two points in time.
// (Same reason a speedometer needs time to compute speed, not just distance.)
static std::vector<ProcessInfo> take_snapshot(ProcessReader& reader) {
    reader.read_all();  // first read: stores baseline tick counts, CPU% comes back 0
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return reader.read_all();  // second read: now CPU% = (new ticks - old ticks) / 1 second
}

// Prints all processes as a table, sorted by CPU% (highest first).
// If `filter` is given, only process names containing that string are shown.
static void print_table(std::vector<ProcessInfo> procs,
                        const std::map<int, int>& conns,
                        const std::string& filter = "") {
    // sort so the most CPU-hungry process appears at the top
    std::sort(procs.begin(), procs.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.cpu_percent > b.cpu_percent;
              });

    // print header row
    std::cout << std::left
              << std::setw(8)  << "PID"
              << std::setw(22) << "NAME"
              << std::setw(8)  << "CPU%"
              << std::setw(10) << "MEM(MB)"
              << std::setw(7)  << "CONNS" << "\n";
    std::cout << std::string(55, '-') << "\n";

    int shown = 0;
    for (const auto& p : procs) {
        // skip processes that don't match the optional filter
        if (!filter.empty() && p.name.find(filter) == std::string::npos)
            continue;

        // look up connection count — 0 if we couldn't read it (permission)
        int c = 0;
        auto it = conns.find(p.pid);
        if (it != conns.end()) c = it->second;

        std::cout << std::left << std::fixed << std::setprecision(1)
                  << std::setw(8)  << p.pid
                  << std::setw(22) << p.name.substr(0, 20)  // truncate very long names
                  << std::setw(8)  << p.cpu_percent
                  << std::setw(10) << p.memory_rss_mb
                  << std::setw(7)  << c << "\n";
        shown++;
    }

    if (shown == 0 && !filter.empty())
        std::cout << "no process matching '" << filter << "' found\n";
    else
        std::cout << "(" << shown << " processes)\n";
}

// Reads the last 10 lines from the alerts log and prints them.
// If the file doesn't exist yet, it means the anomaly detector hasn't run.
static void show_alerts(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cout << "no alerts yet (" << path << " not found)\n";
        std::cout << "is the anomaly detector running? ->  "
                  << "python3 ml/anomaly_detector.py\n";
        return;
    }

    // use a deque as a sliding window — keep only the last 10 lines
    std::deque<std::string> last;
    std::string line;
    while (std::getline(file, line)) {
        last.push_back(line);
        if (last.size() > 10) last.pop_front();  // drop the oldest when we exceed 10
    }

    if (last.empty()) {
        std::cout << "no alerts yet — that's a good sign :)\n";
        return;
    }
    for (const auto& l : last) std::cout << l << "\n";
}

// The core daemon loop: wake up every `interval` seconds, read all process
// stats, write them to the JSON log, and go back to sleep — forever.
//
// Why the warm-up read before the loop?
// CPU% is a delta (change over time). Without a previous reading, the first
// snapshot would report 0% for everything. The warm-up read creates that
// "previous reading" so the first logged snapshot has real numbers.
static void run_daemon(int interval, const std::string& output) {
    ProcessReader reader;
    NetworkTracker tracker;

    std::cout << "endpoint-monitor started\n"
              << "  sampling every " << interval << "s -> " << output << "\n"
              << "  press Ctrl+C to stop\n\n";

    // warm-up: establishes baseline tick counts so CPU% works on first real read
    reader.read_all();

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));

        auto procs = reader.read_all();          // Layer 1: CPU, memory, disk
        auto conns = tracker.count_connections(); // Layer 2: network connections
        write_metrics(output, procs, conns);      // Layer 3: write to JSON log

        // std::endl flushes the buffer — without it, output might not appear
        // if stdout is piped to another program
        std::cout << "[" << now_string() << "] logged "
                  << procs.size() << " processes" << std::endl;
    }
}

static void print_help() {
    std::cout <<
        "endpoint-monitor — per-process CPU/memory/disk/network logger\n\n"
        "  ./monitor                   run the daemon (default)\n"
        "  ./monitor --interval <n>    sample every n seconds (default 5)\n"
        "  ./monitor --output <path>   log file (default logs/metrics.json)\n"
        "  ./monitor --status          one-shot table of all processes\n"
        "  ./monitor --process <name>  show only matching processes\n"
        "  ./monitor --alerts          print the last 10 anomaly alerts\n"
        "  ./monitor --help            this text\n";
}

int main(int argc, char* argv[]) {
    int interval = 5;
    std::string output      = "logs/metrics.json";
    std::string alerts_path = "logs/alerts.log";

    // mode decides what main() does after parsing arguments
    enum class Mode { Daemon, Status, Alerts, Process };
    Mode mode = Mode::Daemon;
    std::string process_filter;

    // simple argument parser — no external library needed for this few flags
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--status") {
            mode = Mode::Status;
        } else if (arg == "--alerts") {
            mode = Mode::Alerts;
        } else if (arg == "--process" && i + 1 < argc) {
            mode = Mode::Process;
            process_filter = argv[++i];  // consume the next argument as the filter
        } else if (arg == "--interval" && i + 1 < argc) {
            interval = std::stoi(argv[++i]);
        } else if (arg == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else {
            std::cout << "unknown option: " << arg << "\n\n";
            print_help();
            return 1;
        }
    }

    // route to the right behavior based on mode
    if (mode == Mode::Alerts) {
        show_alerts(alerts_path);
        return 0;
    }

    if (mode == Mode::Status || mode == Mode::Process) {
        ProcessReader reader;
        NetworkTracker tracker;
        auto procs = take_snapshot(reader);       // two reads, 1s apart
        auto conns = tracker.count_connections();
        print_table(procs, conns, process_filter);
        return 0;
    }

    // default: run as daemon until Ctrl+C
    run_daemon(interval, output);
    return 0;
}
