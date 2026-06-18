#include "json_writer.h"

#include <fstream>
#include <iomanip>
#include <ctime>
#include <filesystem>

// Returns the current time as a human-readable string: "2025-11-14 14:32:01"
// Used as the timestamp field in every JSON line we write.
std::string now_string() {
    time_t t = time(nullptr);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return buf;
}

// If a process name contains a quote or backslash, it would break the JSON.
// This function adds a backslash before those characters so the output stays valid.
// Example: name containing " becomes \"
static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

// Appends one JSON object per process to the log file.
//
// Why append mode (std::ios::app)?
// The daemon runs forever — we never want to overwrite what we already logged.
// Append mode means each call adds new lines at the end, like a growing diary.
//
// Why one JSON object per line (JSON Lines format)?
// The Python script reads this file like `tail -f` — one line at a time.
// If we wrote one big JSON array, we'd have to wait for the file to close
// before parsing it. With JSON Lines, every line is a complete, parseable object.
void write_metrics(const std::string& path,
                   const std::vector<ProcessInfo>& processes,
                   const std::map<int, int>& connections) {
    // create the logs/ directory if it doesn't exist yet
    std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    // open in append mode so we add to the file, not overwrite it
    std::ofstream file(path, std::ios::app);
    file << std::fixed << std::setprecision(2);

    std::string ts = now_string();

    for (const auto& proc : processes) {
        // if we couldn't read a process's connections (permission denied), default to 0
        int conns = 0;
        auto it = connections.find(proc.pid);
        if (it != connections.end()) conns = it->second;

        // write one JSON object on one line — the Python side reads it with json.loads()
        file << "{"
             << "\"timestamp\": \""    << ts                       << "\", "
             << "\"pid\": "            << proc.pid                  << ", "
             << "\"name\": \""         << escape(proc.name)         << "\", "
             << "\"cpu_percent\": "    << proc.cpu_percent          << ", "
             << "\"memory_rss_mb\": "  << proc.memory_rss_mb        << ", "
             << "\"disk_read_bytes\": "<< proc.disk_read_bytes      << ", "
             << "\"disk_write_bytes\":"<< proc.disk_write_bytes     << ", "
             << "\"open_connections\": "<< conns
             << "}\n";
    }
}
