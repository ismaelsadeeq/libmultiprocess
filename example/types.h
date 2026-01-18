// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef EXAMPLE_TYPES_H
#define EXAMPLE_TYPES_H

#include <calculator.capnp.proxy-types.h>
#include <printer.capnp.proxy-types.h>
#include <analytics.capnp.proxy-types.h>

// IWYU pragma: begin_exports
#include <mp/type-context.h>
#include <mp/type-decay.h>
#include <mp/type-interface.h>
#include <mp/type-string.h>
#include <mp/type-threadmap.h>

#include <mp/proxy-types.h>
#include <mp/type-string.h>
#include <mp/type-vector.h>
// IWYU pragma: end_exports

struct InitInterface; // IWYU pragma: export
struct CalculatorInterface; // IWYU pragma: export
struct PrinterInterface; // IWYU pragma: export
struct AnalyticsInterface; // IWYU pragma: export

namespace mp {

// Serialize ProcessInfo struct
template <typename Value, typename Output>
void CustomBuildField(TypeList<ProcessInfo>, Priority<1>, InvokeContext& invoke_context, 
                      Value&& value, Output&& output)
{
    auto builder = output.init();
    builder.setName(value.name);
    builder.setPid(value.pid);
    builder.setCpuPercent(value.cpuPercent);
    builder.setMemoryKB(value.memoryKB);
}

// Deserialize ProcessInfo struct
template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<ProcessInfo>, Priority<1>, InvokeContext& invoke_context,
                               Input&& input, ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        if (!input.has()) return;
        auto reader = input.get();
        
        value.name = reader.getName();
        value.pid = reader.getPid();
        value.cpuPercent = reader.getCpuPercent();
        value.memoryKB = reader.getMemoryKB();
    });
}

// Serialize AnalyticsSnapShot struct
template <typename Value, typename Output>
void CustomBuildField(TypeList<AnalyticsSnapShot>, Priority<1>, InvokeContext& invoke_context, 
                      Value&& value, Output&& output)
{
    auto builder = output.init();
    builder.setTimestamp(value.timestamp);
    builder.setCpuUsagePercent(value.cpuUsagePercent);
    builder.setTotalMemoryMB(value.totalMemoryMB);
    builder.setAvailableMemoryMB(value.availableMemoryMB);
    builder.setUsedMemoryMB(value.usedMemoryMB);
    builder.setCachedMemoryMB(value.cachedMemoryMB);
    builder.setBuffersMemoryMB(value.buffersMemoryMB);
    builder.setProcessCount(value.processCount);
    
    // Serialize the vector of ProcessInfo using libmultiprocess's vector support
    auto processes = builder.initTopProcesses(value.topProcesses.size());
    for (size_t i = 0; i < value.topProcesses.size(); ++i) {
        auto proc = processes[i];
        proc.setName(value.topProcesses[i].name);
        proc.setPid(value.topProcesses[i].pid);
        proc.setCpuPercent(value.topProcesses[i].cpuPercent);
        proc.setMemoryKB(value.topProcesses[i].memoryKB);
    }
}

// Deserialize AnalyticsSnapShot struct
template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<AnalyticsSnapShot>, Priority<1>, InvokeContext& invoke_context,
                               Input&& input, ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        if (!input.has()) return;
        auto reader = input.get();
        
        value.timestamp = reader.getTimestamp();
        value.cpuUsagePercent = reader.getCpuUsagePercent();
        value.totalMemoryMB = reader.getTotalMemoryMB();
        value.availableMemoryMB = reader.getAvailableMemoryMB();
        value.usedMemoryMB = reader.getUsedMemoryMB();
        value.cachedMemoryMB = reader.getCachedMemoryMB();
        value.buffersMemoryMB = reader.getBuffersMemoryMB();
        value.processCount = reader.getProcessCount();
        
        // Deserialize the vector of ProcessInfo
        auto processes = reader.getTopProcesses();
        value.topProcesses.clear();
        value.topProcesses.reserve(processes.size());
        
        for (auto proc : processes) {
            ProcessInfo info;
            info.name = proc.getName();
            info.pid = proc.getPid();
            info.cpuPercent = proc.getCpuPercent();
            info.memoryKB = proc.getMemoryKB();
            value.topProcesses.push_back(std::move(info));
        }
    });
}

}

#endif // EXAMPLE_TYPES_H
