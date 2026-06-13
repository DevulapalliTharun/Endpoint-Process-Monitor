#ifndef JSON_WRITER_H
#define JSON_WRITER_H

#include <string>
#include <vector>
#include <map>

#include "process_reader.h"

// Appends one JSON object per process (one per line) to the log file.
// One-object-per-line ("JSON Lines") keeps things simple: the Python
// side just calls json.loads() on each line instead of parsing one
// giant ever-growing array.
void write_metrics(const std::string& path,
                   const std::vector<ProcessInfo>& processes,
                   const std::map<int, int>& connections);

// "2025-11-14 14:32:01" style timestamp for the current moment.
std::string now_string();

#endif
