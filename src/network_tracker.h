#ifndef NETWORK_TRACKER_H
#define NETWORK_TRACKER_H

#include <map>

// Counts open TCP/UDP connections per process.
//
// How it works: every open socket on the system has a unique inode
// number listed in /proc/net/tcp and /proc/net/udp. Each process's
// open sockets show up in /proc/[pid]/fd as symlinks that look like
// "socket:[12345]". Matching the inode numbers links a connection
// back to the process that owns it.
class NetworkTracker {
public:
    // Returns pid -> number of open TCP/UDP sockets.
    // Without root we can only inspect our own processes' fds;
    // others are silently skipped (their count stays 0).
    std::map<int, int> count_connections();
};

#endif
