#include "network_tracker.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

// Pulls the socket inode numbers out of one /proc/net/* file.
// Every line after the header is one connection; the inode is column 10.
static void collect_socket_inodes(const std::string& path, std::set<long>& inodes) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);  // skip the header row

    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::vector<std::string> fields;
        std::string token;
        while (ss >> token) fields.push_back(token);
        if (fields.size() >= 10)
            inodes.insert(std::stol(fields[9]));
    }
}

std::map<int, int> NetworkTracker::count_connections() {
    // Step 1: gather every socket inode currently open on the system
    std::set<long> inodes;
    collect_socket_inodes("/proc/net/tcp", inodes);
    collect_socket_inodes("/proc/net/udp", inodes);
    collect_socket_inodes("/proc/net/tcp6", inodes);
    collect_socket_inodes("/proc/net/udp6", inodes);

    // Step 2: walk every process's open file descriptors and see which
    // of those inodes it holds.
    std::map<int, int> counts;

    for (const auto& proc_entry : fs::directory_iterator("/proc")) {
        std::string dirname = proc_entry.path().filename().string();
        if (dirname.empty() || !std::isdigit(dirname[0])) continue;
        int pid = std::stoi(dirname);

        // Reading another user's fd folder needs root, so this often
        // fails — we skip those processes instead of crashing.
        std::error_code ec;
        fs::directory_iterator fd_iter(proc_entry.path() / "fd", ec);
        if (ec) continue;

        for (const auto& fd_entry : fd_iter) {
            std::error_code link_ec;
            fs::path target = fs::read_symlink(fd_entry.path(), link_ec);
            if (link_ec) continue;

            // a socket fd looks like: socket:[36063]
            std::string t = target.string();
            if (t.rfind("socket:[", 0) == 0) {
                long inode = std::stol(t.substr(8));
                if (inodes.count(inode)) counts[pid]++;
            }
        }
    }

    return counts;
}
