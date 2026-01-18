#include <analytics.h>
#include <init.capnp.h>
#include <init.capnp.proxy.h> // NOLINT(misc-include-cleaner) // IWYU pragma: keep

#include <charconv>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <kj/async.h>
#include <kj/common.h>
#include <kj/memory.h>
#include <memory>
#include <mp/proxy-io.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <atomic>
#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#include <exception>


class SystemMonitor {
private:
    std::vector<AnalyticsSnapShot> snapshots;
    std::mutex dataMutex;
    std::atomic<bool> running{true};
    std::thread monitorThread;
    int collectionIntervalSeconds;
    
    // CPU tracking
    unsigned long long lastTotalUser, lastTotalUserLow, lastTotalSys, lastTotalIdle;

    void initCPUTracking() {
        std::ifstream file("/proc/stat");
        std::string cpu;
        file >> cpu >> lastTotalUser >> lastTotalUserLow >> lastTotalSys >> lastTotalIdle;
    }

    double getCPUUsage() {
        std::ifstream file("/proc/stat");
        std::string cpu;
        unsigned long long totalUser, totalUserLow, totalSys, totalIdle;
        
        file >> cpu >> totalUser >> totalUserLow >> totalSys >> totalIdle;
        
        if (totalUser < lastTotalUser || totalUserLow < lastTotalUserLow ||
            totalSys < lastTotalSys || totalIdle < lastTotalIdle) {
            return -1.0;
        }
        
        unsigned long long total = (totalUser - lastTotalUser) + 
                                   (totalUserLow - lastTotalUserLow) +
                                   (totalSys - lastTotalSys);
        double percent = total;
        total += (totalIdle - lastTotalIdle);
        percent = (total > 0) ? (percent / total) * 100.0 : 0.0;

        lastTotalUser = totalUser;
        lastTotalUserLow = totalUserLow;
        lastTotalSys = totalSys;
        lastTotalIdle = totalIdle;

        return percent;
    }

    void getMemoryInfo(unsigned long& total, unsigned long& available, 
                       unsigned long& cached, unsigned long& buffers) {
        std::ifstream file("/proc/meminfo");
        std::string line;
        
        unsigned long memTotal = 0, memFree = 0, memCached = 0, memBuffers = 0;
        
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string key;
            unsigned long value;
            std::string unit;
            
            if (iss >> key >> value >> unit) {
                if (key == "MemTotal:") memTotal = value;
                else if (key == "MemFree:") memFree = value;
                else if (key == "Cached:") memCached = value;
                else if (key == "Buffers:") memBuffers = value;
            }
        }
        
        total = memTotal / 1024;
        available = memFree / 1024;
        cached = memCached / 1024;
        buffers = memBuffers / 1024;
    }

    std::vector<ProcessInfo> getTopProcesses(int count = 10) {
        std::vector<ProcessInfo> processes;
        DIR* dir = opendir("/proc");
        if (!dir) return processes;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR) {
                std::string pidStr = entry->d_name;
                if (std::all_of(pidStr.begin(), pidStr.end(), ::isdigit)) {
                    ProcessInfo info;
                    info.pid = std::stoul(pidStr);
                    info.cpuPercent = 0.0;
                    
                    // Get process name
                    std::string statPath = "/proc/" + pidStr + "/stat";
                    std::ifstream statFile(statPath);
                    if (statFile.is_open()) {
                        std::string line;
                        std::getline(statFile, line);
                        
                        size_t start = line.find('(');
                        size_t end = line.rfind(')');
                        if (start != std::string::npos && end != std::string::npos) {
                            info.name = line.substr(start + 1, end - start - 1);
                        }
                    }
                    
                    // Get memory usage
                    std::string statusPath = "/proc/" + pidStr + "/status";
                    std::ifstream statusFile(statusPath);
                    std::string statusLine;
                    info.memoryKB = 0;
                    
                    while (std::getline(statusFile, statusLine)) {
                        if (statusLine.find("VmRSS:") == 0) {
                            std::istringstream iss(statusLine);
                            std::string label;
                            iss >> label >> info.memoryKB;
                            break;
                        }
                    }
                    
                    if (!info.name.empty() && info.memoryKB > 0) {
                        processes.push_back(info);
                    }
                }
            }
        }
        closedir(dir);

        std::sort(processes.begin(), processes.end(),
                  [](const ProcessInfo& a, const ProcessInfo& b) {
                      return a.memoryKB > b.memoryKB;
                  });

        if (processes.size() > static_cast<size_t>(count)) {
            processes.resize(count);
        }
        return processes;
    }

    int getProcessCount() {
        int count = 0;
        DIR* dir = opendir("/proc");
        if (!dir) return 0;

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_DIR) {
                std::string pidStr = entry->d_name;
                if (std::all_of(pidStr.begin(), pidStr.end(), ::isdigit)) {
                    count++;
                }
            }
        }
        closedir(dir);
        return count;
    }

    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

    void monitorLoop() {
        initCPUTracking();
        
        // Initial sleep to get baseline CPU stats
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        while (running) {
            AnalyticsSnapShot snapshot;
            snapshot.timestamp = getCurrentTimestamp();
            snapshot.cpuUsagePercent = getCPUUsage();
            
            getMemoryInfo(snapshot.totalMemoryMB, snapshot.availableMemoryMB,
                         snapshot.cachedMemoryMB, snapshot.buffersMemoryMB);
            snapshot.usedMemoryMB = snapshot.totalMemoryMB - snapshot.availableMemoryMB - 
                                   snapshot.cachedMemoryMB - snapshot.buffersMemoryMB;
            
            snapshot.topProcesses = getTopProcesses(10);
            snapshot.processCount = getProcessCount();
            printSnapshot(snapshot);

            {
                std::lock_guard<std::mutex> lock(dataMutex);
                snapshots.push_back(snapshot);
                
            }

            std::this_thread::sleep_for(std::chrono::seconds(collectionIntervalSeconds));
        }
    }
    void printSnapshot(const AnalyticsSnapShot& snapshot) {
        std::cout << "\n=== System Snapshot at " << snapshot.timestamp << " ===\n";
        std::cout << "CPU Usage: " << std::fixed << std::setprecision(2) 
                  << snapshot.cpuUsagePercent << "%\n";
        std::cout << "Total Processes: " << snapshot.processCount << "\n";
        std::cout << "Memory:\n";
        std::cout << "  Total:     " << snapshot.totalMemoryMB << " MB\n";
        std::cout << "  Used:      " << snapshot.usedMemoryMB << " MB\n";
        std::cout << "  Available: " << snapshot.availableMemoryMB << " MB\n";
        std::cout << "  Cached:    " << snapshot.cachedMemoryMB << " MB\n";
        std::cout << "  Buffers:   " << snapshot.buffersMemoryMB << " MB\n";
        
        std::cout << "\nTop Processes by Memory:\n";
        for (const auto& proc : snapshot.topProcesses) {
            std::cout << "  " << std::setw(6) << proc.pid << " | " 
                      << std::setw(30) << std::left << proc.name << " | "
                      << std::setw(10) << std::right << proc.memoryKB << " KB\n";
        }
    }


public:
    SystemMonitor(int intervalSeconds = 5) : collectionIntervalSeconds(intervalSeconds) {}

    void start() {
        running = true;
        monitorThread = std::thread(&SystemMonitor::monitorLoop, this);
    }

    void stop() {
        running = false;
        if (monitorThread.joinable()) {
            monitorThread.join();
        }
    }

    AnalyticsSnapShot getCurrentSnapshot() {
        AnalyticsSnapShot snapshot;
        snapshot.timestamp = getCurrentTimestamp();
        snapshot.cpuUsagePercent = getCPUUsage();
        
        getMemoryInfo(snapshot.totalMemoryMB, snapshot.availableMemoryMB,
                     snapshot.cachedMemoryMB, snapshot.buffersMemoryMB);
        snapshot.usedMemoryMB = snapshot.totalMemoryMB - snapshot.availableMemoryMB - 
                               snapshot.cachedMemoryMB - snapshot.buffersMemoryMB;
        
        snapshot.topProcesses = getTopProcesses(10);
        snapshot.processCount = getProcessCount();
        
        return snapshot;
    }

    std::vector<AnalyticsSnapShot> getAllSnapshots() {
        std::lock_guard<std::mutex> lock(dataMutex);
        return snapshots;
    }

    void clearSnapshots() {
        std::lock_guard<std::mutex> lock(dataMutex);
        snapshots.clear();
    }

    size_t getSnapshotCount() {
        std::lock_guard<std::mutex> lock(dataMutex);
        return snapshots.size();
    }

    ~SystemMonitor() {
        stop();
    }
};

class AnalyticsImpl: public Analytics {

std::unique_ptr<SystemMonitor> monitor;
public:
    AnalyticsImpl() : monitor(std::make_unique<SystemMonitor>(6))
    {
        monitor->start();
    };
    AnalyticsSnapShot getAnalytics() { return monitor->getCurrentSnapshot(); }
}; 

class InitImpl : public Init
{
public:
    std::unique_ptr<Analytics> makeAnalytics() override
    {
        return std::make_unique<AnalyticsImpl>();
    }
};


static void LogPrint(mp::LogMessage log_data)
{
    if (log_data.level == mp::Log::Raise) throw std::runtime_error(log_data.message);
    std::ofstream("debug.log", std::ios_base::app) << log_data.message << std::endl;
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cout << "Usage: mpanalytics <fd>\n";
        return 1;
    }
    int fd;
    if (std::from_chars(argv[1], argv[1] + strlen(argv[1]), fd).ec != std::errc{}) {
        std::cerr << argv[1] << " is not a number or is larger than an int\n";
        return 1;
    }
    try {
        std::cout << "Using the analytics server.\n";
        mp::EventLoop loop("mpprinter", LogPrint);
        std::unique_ptr<Init> init = std::make_unique<InitImpl>();
        mp::ServeStream<InitInterface>(loop, fd, *init);
        loop.loop();
    } catch (std::exception& e) {
        std::cout << e.what() << std::endl;
    }
    return 0;
}
