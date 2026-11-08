// System information for the debug displays and debug dumps: this process's resource use,
// hardware/software descriptions, and removal of personal details from dump text.
#pragma once

#include <cstdint>
#include <string>

struct ResourceSample {
    bool ok = false;
    double cpuSeconds = 0;        // user + system CPU time used by the process so far
    uint64_t memoryBytes = 0;     // resident memory (working set on Windows)
    uint64_t readBytes = 0;       // bytes read so far (files, including cached reads)
    uint64_t writeBytes = 0;      // bytes written so far
    int threads = 0;              // 0 = unknown
};
// Reads the current totals (cheap; meant for a few calls per second).
ResourceSample sampleResources();
int logicalCpus();
std::string cpuName();               // e.g. "AMD Ryzen 7 1700X Eight-Core Processor" ("" if unknown)
uint64_t memoryAvailableBytes();     // free + reclaimable RAM (0 if unknown)

// "key = value" lines for debug dumps. `gl*` come from the caller (needs a current context).
// `hideSerials`: drop monitor serial numbers from display names (with the dump's redaction).
std::string hardwareInfo(const char *glVendor, const char *glRenderer, bool hideSerials = false);
// A display name without its serial number ("make model <serial> (connector)").
std::string withoutSerial(const std::string &name);
std::string softwareInfo(const char *glVersion, const char *glslVersion);

// Replaces the user name, home folder (-> ~), computer name, IPv4/IPv6/MAC addresses and
// e-mail addresses in `text`.
std::string redactPersonal(const std::string &text);
