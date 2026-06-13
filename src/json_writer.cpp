#include "json_writer.h"

#include <fstream>
#include <iomanip>
#include <ctime>
#include <filesystem>

std::string now_string() {
    time_t t = time(nullptr);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
    return buf;
}

// Process names are usually plain words, but escape quotes and
// backslashes anyway so we never produce broken JSON.
static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void write_metrics(const std::string& path,
                   const std::vector<ProcessInfo>& processes,
                   const std::map<int, int>& connections) {
    // make sure the logs/ folder exists before writing into it
    std::filesystem::path p(path);
    if (p.has_parent_path())
        std::filesystem::create_directories(p.parent_path());

    std::ofstream file(path, std::ios::app);
    file << std::fixed << std::setprecision(2);

    std::string ts = now_string();

    for (const auto& proc : processes) {
        // connection count is 0 if the tracker couldn't see this PID
        int conns = 0;
        auto it = connections.find(proc.pid);
        if (it != connections.end()) conns = it->second;

        file << "{"
             << "\"timestamp\": \"" << ts << "\", "
             << "\"pid\": " << proc.pid << ", "
             << "\"name\": \"" << escape(proc.name) << "\", "
             << "\"cpu_percent\": " << proc.cpu_percent << ", "
             << "\"memory_rss_mb\": " << proc.memory_rss_mb << ", "
             << "\"disk_read_bytes\": " << proc.disk_read_bytes << ", "
             << "\"disk_write_bytes\": " << proc.disk_write_bytes << ", "
             << "\"open_connections\": " << conns
             << "}\n";
    }
}
