// endpoint-monitor
//
// A small daemon that watches every process on the machine and logs
// its CPU / memory / disk / network activity as JSON every few seconds.
// The Python script in ml/ reads that log and flags anomalies.
//
// Usage:
//   ./monitor                          run the daemon (5s interval)
//   ./monitor --interval 10            custom sampling interval
//   ./monitor --output logs/m.json     custom log path
//   ./monitor --status                 one-shot table of all processes
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

// Takes two snapshots one second apart. CPU% is a rate — it needs a
// "before" and an "after" to mean anything, so a single read won't do.
static std::vector<ProcessInfo> take_snapshot(ProcessReader& reader) {
    reader.read_all();  // warm-up read, values are all zero
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return reader.read_all();
}

// Prints processes as a table, highest CPU first. If `filter` is given,
// only names containing that text are shown.
static void print_table(std::vector<ProcessInfo> procs,
                        const std::map<int, int>& conns,
                        const std::string& filter = "") {
    std::sort(procs.begin(), procs.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.cpu_percent > b.cpu_percent;
              });

    std::cout << std::left
              << std::setw(8)  << "PID"
              << std::setw(22) << "NAME"
              << std::setw(8)  << "CPU%"
              << std::setw(10) << "MEM(MB)"
              << std::setw(7)  << "CONNS" << "\n";
    std::cout << std::string(55, '-') << "\n";

    int shown = 0;
    for (const auto& p : procs) {
        if (!filter.empty() && p.name.find(filter) == std::string::npos)
            continue;

        int c = 0;
        auto it = conns.find(p.pid);
        if (it != conns.end()) c = it->second;

        std::cout << std::left << std::fixed << std::setprecision(1)
                  << std::setw(8)  << p.pid
                  << std::setw(22) << p.name.substr(0, 20)
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

// Prints the last 10 lines of the alerts file.
static void show_alerts(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        std::cout << "no alerts yet (" << path << " not found)\n";
        std::cout << "is the anomaly detector running? ->  "
                  << "python3 ml/anomaly_detector.py\n";
        return;
    }

    std::deque<std::string> last;
    std::string line;
    while (std::getline(file, line)) {
        last.push_back(line);
        if (last.size() > 10) last.pop_front();
    }

    if (last.empty()) {
        std::cout << "no alerts yet — that's a good sign :)\n";
        return;
    }
    for (const auto& l : last) std::cout << l << "\n";
}

// The main daemon loop: sample everything, append to the JSON log,
// sleep, repeat — until Ctrl+C.
static void run_daemon(int interval, const std::string& output) {
    ProcessReader reader;
    NetworkTracker tracker;

    std::cout << "endpoint-monitor started\n"
              << "  sampling every " << interval << "s -> " << output << "\n"
              << "  press Ctrl+C to stop\n\n";

    // first read is a warm-up: CPU% needs two samples to compute,
    // so we don't log this one (it would be all zeros)
    reader.read_all();

    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(interval));

        auto procs = reader.read_all();
        auto conns = tracker.count_connections();
        write_metrics(output, procs, conns);

        // endl (not \n) so the message shows up immediately even when
        // stdout is piped or redirected
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
    std::string output = "logs/metrics.json";
    std::string alerts_path = "logs/alerts.log";

    enum class Mode { Daemon, Status, Alerts, Process };
    Mode mode = Mode::Daemon;
    std::string process_filter;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--status") {
            mode = Mode::Status;
        } else if (arg == "--alerts") {
            mode = Mode::Alerts;
        } else if (arg == "--process" && i + 1 < argc) {
            mode = Mode::Process;
            process_filter = argv[++i];
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

    if (mode == Mode::Alerts) {
        show_alerts(alerts_path);
        return 0;
    }

    if (mode == Mode::Status || mode == Mode::Process) {
        ProcessReader reader;
        NetworkTracker tracker;
        auto procs = take_snapshot(reader);
        auto conns = tracker.count_connections();
        print_table(procs, conns, process_filter);
        return 0;
    }

    run_daemon(interval, output);
    return 0;
}
