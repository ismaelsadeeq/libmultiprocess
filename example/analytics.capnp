@0x9be1c1cdfd6e8972;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("analytics::capnp");

using Proxy = import "/mp/proxy.capnp";
$Proxy.include("analytics_snapshot.h");
$Proxy.include("analytics.h");
$Proxy.includeTypes("types.h");



struct ProcessInfo $Proxy.wrap("ProcessInfo") {
    name @0 :Text;
    pid @1 :UInt64;
    cpuPercent @2 :Float64;
    memoryKB @3 :UInt64;
}

struct AnalyticsSnapShot $Proxy.wrap("AnalyticsSnapShot") {
    timestamp @0 :Text;
    cpuUsagePercent @1 :Float64;
    totalMemoryMB @2 :UInt64;
    availableMemoryMB @3 :UInt64;
    usedMemoryMB @4 :UInt64;
    cachedMemoryMB @5 :UInt64;
    buffersMemoryMB @6 :UInt64;
    processCount @7 :Int32;
    topProcesses @8 :List(ProcessInfo);
}

interface AnalyticsInterface $Proxy.wrap("Analytics") {
    getAnalytics @0 (context: Proxy.Context) -> (result: AnalyticsSnapShot); 

}

