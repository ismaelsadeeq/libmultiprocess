#ifndef EXAMPLE_ANALYTICS_SNAPSHOT_H
#define EXAMPLE_ANALYTICS_SNAPSHOT_H

#include <string>
#include <vector>

struct ProcessInfo {
    std::string name;
    unsigned long pid;
    double cpuPercent;
    unsigned long memoryKB;
};

struct AnalyticsSnapShot {
    std::string timestamp;
    double cpuUsagePercent;
    unsigned long totalMemoryMB;
    unsigned long availableMemoryMB;
    unsigned long usedMemoryMB;
    unsigned long cachedMemoryMB;
    unsigned long buffersMemoryMB;
    int processCount;
    std::vector<ProcessInfo> topProcesses;
};

#endif // EXAMPLE_ANALYTICS_SNAPSHOT_H
