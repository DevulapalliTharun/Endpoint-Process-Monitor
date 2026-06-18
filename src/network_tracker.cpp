#include "network_tracker.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

// Reads one /proc/net/* file (tcp, udp, tcp6, or udp6) and collects
// the inode number of every active socket listed in it.
//
// Each line in those files looks like (after the header):
//   sl  local_address  rem_address  st  tx_queue rx_queue  tr tm->when retrnsmt  uid  timeout  inode
// Column index 9 (zero-based) is the inode. That inode number is the key
// we use to link a socket to the process that owns it.
static void collect_socket_inodes(const std::string& path, std::set<long>& inodes) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);  // skip the header row — it's not a real connection

    while (std::getline(file, line)) {
        std::istringstream ss(line);
        std::vector<std::string> fields;
        std::string token;
        while (ss >> token) fields.push_back(token);

        // column 9 is the inode — add it to our global set
        if (fields.size() >= 10)
            inodes.insert(std::stol(fields[9]));
    }
}

// Returns a map of { pid -> number of open TCP/UDP connections }.
//
// Big picture — two steps:
//   Step 1: collect all socket inodes from /proc/net/tcp + udp + tcp6 + udp6
//   Step 2: for each process, scan /proc/[pid]/fd/ for symlinks that look like
//           "socket:[inode]" — if that inode is in our set, this process owns it
//
// Why inodes? Because /proc/net/* and /proc/[pid]/fd/* both use the same inode
// number to refer to the same socket — it's the only common identifier.
std::map<int, int> NetworkTracker::count_connections() {
    // Step 1: gather every socket inode currently open on the whole system
    std::set<long> inodes;
    collect_socket_inodes("/proc/net/tcp",  inodes);  // IPv4 TCP
    collect_socket_inodes("/proc/net/udp",  inodes);  // IPv4 UDP
    collect_socket_inodes("/proc/net/tcp6", inodes);  // IPv6 TCP
    collect_socket_inodes("/proc/net/udp6", inodes);  // IPv6 UDP

    // Step 2: walk every process's open file descriptors
    std::map<int, int> counts;

    for (const auto& proc_entry : fs::directory_iterator("/proc")) {
        // only look at numeric folders (those are PIDs)
        std::string dirname = proc_entry.path().filename().string();
        if (dirname.empty() || !std::isdigit(dirname[0])) continue;
        int pid = std::stoi(dirname);

        // /proc/[pid]/fd/ can only be read by root or the process owner.
        // std::error_code lets us skip silently instead of crashing.
        std::error_code ec;
        fs::directory_iterator fd_iter(proc_entry.path() / "fd", ec);
        if (ec) continue;  // permission denied — skip this PID

        for (const auto& fd_entry : fd_iter) {
            // each entry in fd/ is a symlink to the actual resource.
            // sockets look like:  socket:[36063]
            // files look like:    /home/user/file.txt  (we ignore these)
            std::error_code link_ec;
            fs::path target = fs::read_symlink(fd_entry.path(), link_ec);
            if (link_ec) continue;

            std::string t = target.string();
            if (t.rfind("socket:[", 0) == 0) {
                // extract the inode number from "socket:[12345]"
                long inode = std::stol(t.substr(8));  // skip "socket:["
                if (inodes.count(inode))
                    counts[pid]++;  // this process owns one more socket
            }
        }
    }

    return counts;  // pid -> count, only includes PIDs with at least 1 connection
}
