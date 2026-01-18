# Complete Guide to IPC Development with libmultiprocess and Cap'n Proto

## Table of Contents
- [Introduction](#introduction)
- [Core Concepts](#core-concepts)
- [Detailed Lessons](#detailed-lessons)
- [Analytics Monitor Example](#analytics-monitor-example)
- [Common Pitfalls Reference](#common-pitfalls-reference)
- [Best Practices](#best-practices)

---

## Introduction

This guide documents the complete process of building an Inter-Process Communication (IPC) system using libmultiprocess and Cap'n Proto, based on the Bitcoin Core architecture. We built a system analytics monitor that demonstrates all key concepts of cross-process communication in C++.

**What you'll learn:**
- How to design Cap'n Proto schemas for IPC
- Custom type serialization between C++ and Cap'n Proto
- Build system integration with CMake
- Process communication via file descriptors
- Debugging strategies for IPC systems
- Production-grade patterns from Bitcoin Core

---

## Core Concepts

### What is IPC?

Inter-Process Communication allows separate operating system processes to exchange data and invoke methods on objects living in other processes. This enables:

- **Process isolation**: Crashes in one process don't affect others
- **Security boundaries**: Separate privilege levels between processes
- **Resource management**: Different memory and CPU limits per process
- **Language interop**: Different languages can communicate through IPC

### The Technology Stack

**Cap'n Proto**: A serialization format and RPC system that provides:
- Zero-copy deserialization
- Type-safe schema evolution
- Efficient binary encoding
- Language-independent interface definitions

**libmultiprocess**: Bitcoin Core's wrapper around Cap'n Proto that provides:
- Automatic proxy code generation
- Type conversion between C++ and Cap'n Proto
- Connection management and lifecycle handling
- Integration with Bitcoin Core's architecture

### How It Works

```
Process A (Client)              Process B (Server)
┌─────────────────┐            ┌─────────────────┐
│  C++ Object     │            │  C++ Object     │
│  (Proxy)        │            │  (Implementation)│
└────────┬────────┘            └────────▲────────┘
         │                              │
    ┌────▼──────────┐            ┌─────┴──────────┐
    │ Proxy Client  │            │ Proxy Server   │
    │ (Generated)   │            │ (Generated)    │
    └────────┬──────┘            └─────▲──────────┘
             │                          │
        ┌────▼──────────────────────────┴────┐
        │    Cap'n Proto RPC over fd         │
        │    (File Descriptor / Socket)      │
        └────────────────────────────────────┘
```

1. Client calls method on proxy object
2. Proxy client serializes call to Cap'n Proto message
3. Message sent over file descriptor to server process
4. Proxy server deserializes and invokes actual C++ method
5. Response serialized and sent back to client
6. Client proxy deserializes and returns to caller

---

## Detailed Lessons

### 1. Cap'n Proto Schema Fundamentals

#### Schema Structure

Every Cap'n Proto schema file requires:

```capnp
# Unique 64-bit identifier (generate with capnp id)
@0x9be1c1cdfd6e8972;

# C++ namespace for generated code
using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("analytics::capnp");

# libmultiprocess proxy utilities
using Proxy = import "/mp/proxy.capnp";
$Proxy.include("analytics_snapshot.h");
$Proxy.includeTypes("types.h");
```

**The unique identifier** is permanent. It's used for type checking across schema versions and must never change for a given schema file.

**Namespace annotations** control where generated C++ code lives. Choose namespaces that don't conflict with your application code.

**Include directives** tell the code generator which headers to include in the generated proxy code.

#### Type Mappings

| C++ Type | Cap'n Proto Type | Notes |
|----------|------------------|-------|
| `std::string` | `Text` | UTF-8 encoded string |
| `std::vector<uint8_t>` | `Data` | Raw bytes |
| `bool` | `Bool` | Boolean value |
| `int32_t`, `uint32_t` | `Int32`, `UInt32` | 32-bit integers |
| `int64_t`, `uint64_t` | `Int64`, `UInt64` | 64-bit integers |
| `float`, `double` | `Float32`, `Float64` | Floating point |
| `std::vector<T>` | `List(T)` | Dynamic arrays |
| Custom struct | `struct` | Requires custom serialization |

#### Field Ordinals

```capnp
struct ProcessInfo $Proxy.wrap("ProcessInfo") {
    name @0 :Text;        # Ordinal 0
    pid @1 :UInt64;       # Ordinal 1
    cpuPercent @2 :Float64;   # Ordinal 2
    memoryKB @3 :UInt64;  # Ordinal 3
}
```

**Ordinals are permanent identifiers** for fields. Once assigned, they must never be reused for a different field. This enables schema evolution:
- Adding new fields with new ordinals is safe
- Removing fields is safe (leave ordinal unused)
- Changing field types breaks compatibility
- Reordering fields in the schema doesn't matter (ordinals define the order)

#### Interface Definitions

```capnp
interface AnalyticsInterface $Proxy.wrap("Analytics") {
    destroy @0 (context: Proxy.Context) -> ();
    getAnalytics @1 (context: Proxy.Context) -> (result: AnalyticsSnapShot);
    startMonitoring @2 (context: Proxy.Context, interval: Int32) -> ();
}
```

**The destroy method** is essential. Objects created in the server process need explicit cleanup. When the client is done with a remote object, it must call destroy to free server-side resources.

**Context parameters** provide access to the IPC connection, enable logging, and support request cancellation. They're required by the libmultiprocess framework.

**Return values** use named fields. Multiple return values are supported by using a struct-like syntax in the return type.

#### Common Schema Pitfalls

**Import path errors:**
```capnp
# WRONG - missing leading slash
using Proxy = import "mp/proxy.capnp";

# CORRECT - absolute path
using Proxy = import "/mp/proxy.capnp";
```

The leading slash indicates an absolute path from the include directories configured in your build system.

**Missing includes:**
```capnp
# If you use custom types, you must include both headers
$Proxy.include("my_types.h");           # C++ type definitions
$Proxy.includeTypes("my_serializers.h"); # Serialization functions
```

Without these, the generated code won't be able to convert between C++ and Cap'n Proto types.

**Ordinal conflicts:**
```capnp
struct BadExample {
    field1 @0 :Text;
    field2 @0 :Int32;  # ERROR: ordinal 0 used twice
}
```

Each field in a struct or method in an interface must have a unique ordinal.

---

### 2. Type Serialization Architecture

#### Why Custom Serialization is Needed

Cap'n Proto natively understands primitive types and standard containers. However, your custom C++ structs are opaque to the framework. Even if a struct contains only standard types, the framework needs explicit instructions on how to map struct fields to Cap'n Proto message fields.

```cpp
// This struct is NOT automatically serializable
struct ProcessInfo {
    std::string name;      // Standard type
    unsigned long pid;     // Standard type
    double cpuPercent;     // Standard type
    unsigned long memoryKB; // Standard type
};
// Even though all fields are standard types,
// the framework doesn't know the struct layout!
```

#### The Serialization Hook System

libmultiprocess uses template specialization to provide customization points:

```cpp
namespace mp {

// Serialization: C++ → Cap'n Proto
template <typename Value, typename Output>
void CustomBuildField(TypeList<ProcessInfo>, Priority<1>, 
                      InvokeContext& invoke_context,
                      Value&& value, Output&& output)
{
    auto builder = output.init();
    builder.setName(value.name);
    builder.setPid(value.pid);
    builder.setCpuPercent(value.cpuPercent);
    builder.setMemoryKB(value.memoryKB);
}

// Deserialization: Cap'n Proto → C++
template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<ProcessInfo>, Priority<1>,
                               InvokeContext& invoke_context,
                               Input&& input, ReadDest&& read_dest)
{
    return read_dest.update([&](auto& value) {
        if (!input.has()) return;  // Check if field is present
        auto reader = input.get();
        
        value.name = reader.getName();
        value.pid = reader.getPid();
        value.cpuPercent = reader.getCpuPercent();
        value.memoryKB = reader.getMemoryKB();
    });
}

} // namespace mp
```

#### Priority Levels

The Priority template parameter creates a hierarchy:

- **Priority<0>**: Framework internals (cv-qualifier removal, reference unwrapping)
- **Priority<1>**: General-purpose custom types (your types go here)
- **Priority<2>**: Higher-priority overrides (special cases)
- **Priority<3>**: Highest priority (framework hooks)

Higher priorities take precedence. This allows:
1. Bitcoin Core's common-types.h to provide generic serialization for any type with Serialize/Unserialize methods
2. Specific types to override with specialized serialization
3. Framework to handle reference types at the lowest level

#### The InvokeContext Parameter

```cpp
void CustomBuildField(TypeList<MyType>, Priority<1>, 
                      InvokeContext& invoke_context,  // <-- This parameter
                      Value&& value, Output&& output)
```

The invoke_context provides:
- Access to the underlying connection
- Request metadata (for logging, metrics)
- Ability to make callbacks to the client
- Support for request cancellation

Most serialization functions don't use it directly, but it's required by the framework's template signatures.

#### Handling Complex Types

**Nested structs:**
```cpp
struct AnalyticsSnapShot {
    std::string timestamp;
    double cpuUsagePercent;
    std::vector<ProcessInfo> topProcesses;  // Nested type
};

void CustomBuildField(TypeList<AnalyticsSnapShot>, Priority<1>,
                      InvokeContext& invoke_context,
                      Value&& value, Output&& output)
{
    auto builder = output.init();
    builder.setTimestamp(value.timestamp);
    builder.setCpuUsagePercent(value.cpuUsagePercent);
    
    // Serialize vector of custom types
    auto processes = builder.initTopProcesses(value.topProcesses.size());
    for (size_t i = 0; i < value.topProcesses.size(); ++i) {
        auto proc = processes[i];
        // Must serialize each ProcessInfo manually
        proc.setName(value.topProcesses[i].name);
        proc.setPid(value.topProcesses[i].pid);
        proc.setCpuPercent(value.topProcesses[i].cpuPercent);
        proc.setMemoryKB(value.topProcesses[i].memoryKB);
    }
}
```

**Optional fields:**
```cpp
struct OptionalData {
    std::string name;
    std::optional<int> age;
};

void CustomBuildField(TypeList<OptionalData>, Priority<1>,
                      InvokeContext& invoke_context,
                      Value&& value, Output&& output)
{
    auto builder = output.init();
    builder.setName(value.name);
    
    if (value.age.has_value()) {
        builder.setAge(*value.age);
    }
    // If optional is empty, field is not set in message
}
```

#### Common Serialization Pitfalls

**Forgetting to check has():**
```cpp
// WRONG - will crash if field not present
decltype(auto) CustomReadField(...) {
    return read_dest.update([&](auto& value) {
        auto reader = input.get();  // Crashes if !input.has()
        value.name = reader.getName();
    });
}

// CORRECT - check before accessing
decltype(auto) CustomReadField(...) {
    return read_dest.update([&](auto& value) {
        if (!input.has()) return;   // Safe guard
        auto reader = input.get();
        value.name = reader.getName();
    });
}
```

**Field name mismatches:**
```cpp
// C++ struct
struct MyData {
    int myField;  // camelCase
};

// Cap'n Proto schema
struct MyData {
    myField @0 :Int32;  # Must match exactly
}

// Serialization - method name must match schema
builder.setMyField(value.myField);  // CORRECT
builder.setMyfield(value.myField);  // WRONG - won't compile
```

**Missing vector iteration:**
```cpp
// WRONG - can't assign vector directly
auto processes = builder.initTopProcesses(value.topProcesses.size());
processes = value.topProcesses;  // ERROR: no assignment operator

// CORRECT - must iterate
for (size_t i = 0; i < value.topProcesses.size(); ++i) {
    auto proc = processes[i];
    // Set each field individually
}
```

---

### 3. Build System Integration

#### CMake Configuration

The `target_capnp_sources` function is the key to integrating Cap'n Proto schemas:

```cmake
add_executable(mpanalytics
  analytics.cpp
)
target_capnp_sources(mpanalytics ${CMAKE_CURRENT_SOURCE_DIR} 
                     init.capnp 
                     analytics.capnp)
target_include_directories(mpanalytics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpanalytics PRIVATE Threads::Threads)
```

**What target_capnp_sources does:**
1. Runs `mpgen` code generator on each .capnp file
2. Generates multiple C++ files per schema:
   - `schema.capnp.h` - Cap'n Proto message types
   - `schema.capnp.c++` - Cap'n Proto message implementation
   - `schema.capnp.proxy-client.c++` - Client proxy code
   - `schema.capnp.proxy-server.c++` - Server proxy code
   - `schema.capnp.proxy-types.h` - Type conversion helpers
   - `schema.capnp.proxy-types.c++` - Type conversion implementation
   - `schema.capnp.proxy.h` - Public proxy interface
3. Adds all generated files to the target's sources
4. Configures include paths for generated headers

#### Transitive Dependencies

If `init.capnp` references multiple interfaces:

```capnp
interface Init {
    makeAnalytics @0 (...) -> (result: AnalyticsInterface);
    makeCalculator @1 (...) -> (result: CalculatorInterface);
    makePrinter @2 (...) -> (result: PrinterInterface);
}
```

Then **every executable that includes init.capnp must also include the schemas for AnalyticsInterface, CalculatorInterface, and PrinterInterface:**

```cmake
target_capnp_sources(mpanalytics ${CMAKE_CURRENT_SOURCE_DIR}
                     init.capnp
                     analytics.capnp
                     calculator.capnp
                     printer.capnp)  # All schemas needed!
```

This ensures the proxy code for all referenced interfaces is compiled and available.

#### One Executable Per Main Function

Each process type needs its own executable:

```cmake
# Analytics server process
add_executable(mpanalytics
  analytics.cpp  # Contains main()
)

# Calculator server process
add_executable(mpcalculator
  calculator.cpp  # Contains main()
)

# DO NOT DO THIS - two main() functions!
add_executable(mpbad
  analytics.cpp   # Has main()
  calculator.cpp  # Also has main() - LINKER ERROR!
)
```

The linker error would be:
```
mold: error: duplicate symbol: main
>>> example/CMakeFiles/mpbad.dir/calculator.cpp.o
>>> example/CMakeFiles/mpbad.dir/analytics.cpp.o
```

#### Common Build Pitfalls

**Missing schema in target_capnp_sources:**

Error:
```
mold: error: undefined symbol: vtable for mp::ProxyServer<AnalyticsInterface>
```

Cause: The `analytics.capnp` file wasn't included in the target's `target_capnp_sources`, so the proxy-server code was never generated or compiled.

Fix: Add `analytics.capnp` to the `target_capnp_sources` call.

**Missing types.h include in schema:**

Error:
```
error: no matching function for call to 'CustomReadField'
note: candidate function template not viable
```

Cause: The schema file doesn't have `$Proxy.includeTypes("types.h")`, so the generated code can't see your CustomBuildField/CustomReadField functions.

Fix: Add `$Proxy.includeTypes("types.h")` to your .capnp file.

**Linking multiple implementations:**

Error:
```
mold: error: duplicate symbol: main
```

Cause: Multiple source files with `main()` functions are being linked into the same executable.

Fix: Each executable should have exactly one source file with `main()`. Create separate executables for each process type.

---

### 4. Inter-Process Communication Architecture

#### File Descriptors Explained

A file descriptor (fd) is an integer that represents an open I/O resource in the operating system. Everything in Unix is a file:
- Regular files (fd 0, 1, 2 are stdin, stdout, stderr)
- Network sockets
- Pipes
- Unix domain sockets

For IPC, we use **socketpair()** which creates two connected file descriptors:

```cpp
int fds[2];
socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
// fds[0] and fds[1] are now connected
// Data written to fds[0] can be read from fds[1]
// Data written to fds[1] can be read from fds[0]
```

#### Process Spawning Pattern

**Parent process:**
```cpp
#include <sys/socket.h>
#include <unistd.h>

int main() {
    // 1. Create socketpair for IPC
    int fds[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    
    // 2. Fork child process
    pid_t pid = fork();
    
    if (pid == 0) {
        // Child process
        close(fds[0]);  // Close parent's end
        
        // 3. Execute child program with fd as argument
        char fd_str[16];
        sprintf(fd_str, "%d", fds[1]);
        execl("./mpanalytics", "mpanalytics", fd_str, nullptr);
        
    } else {
        // Parent process
        close(fds[1]);  // Close child's end
        
        // 4. Use fds[0] to communicate with child
        mp::ServeStream<InitInterface>(loop, fds[0], *init);
    }
}
```

**Child process (analytics.cpp):**
```cpp
int main(int argc, char* argv[]) {
    // 1. Parse fd from command line
    int fd;
    if (std::from_chars(argv[1], argv[1] + strlen(argv[1]), fd).ec != std::errc{}) {
        std::cerr << "Invalid fd argument\n";
        return 1;
    }
    
    // 2. Create event loop
    mp::EventLoop loop("mpanalytics", LogFunction);
    
    // 3. Create service implementation
    std::unique_ptr<Analytics> analytics = std::make_unique<AnalyticsImpl>();
    
    // 4. Serve on the fd passed from parent
    mp::ServeStream<AnalyticsInterface>(loop, fd, *analytics);
    
    // 5. Run event loop (blocks until connection closes)
    loop.loop();
    
    return 0;
}
```

#### Object Lifetime Management

Objects created through IPC have special lifetime considerations:

**Without destroy:**
```cpp
// Client process
auto analytics = init.makeAnalytics();
analytics.getAnalytics();  // Works
// Client exits...
// Server still holds Analytics object - MEMORY LEAK!
```

**With destroy:**
```cpp
// Client process
{
    auto analytics = init.makeAnalytics();
    analytics.getAnalytics();
    analytics.destroy();  // Explicit cleanup
}
// Server frees Analytics object - no leak
```

**Best practice - RAII wrapper:**
```cpp
class AnalyticsGuard {
    Analytics* analytics_;
public:
    AnalyticsGuard(Analytics* a) : analytics_(a) {}
    ~AnalyticsGuard() { 
        if (analytics_) analytics_->destroy(); 
    }
    Analytics* operator->() { return analytics_; }
};

// Usage
auto analytics = AnalyticsGuard(init.makeAnalytics());
analytics->getAnalytics();
// destroy() called automatically on scope exit
```

#### Context Parameters

Every interface method takes a `Proxy.Context` parameter:

```capnp
interface AnalyticsInterface {
    getAnalytics @0 (context: Proxy.Context) -> (result: AnalyticsSnapShot);
    //               ^^^^^^^^^^^^^^^^^^^^
}
```

This context provides:
- Access to the underlying connection
- Request metadata (caller identity, timing)
- Cancellation support (detect if client disconnected)
- Callback capabilities (server can call back to client)

Example usage:
```cpp
AnalyticsSnapShot AnalyticsImpl::getAnalytics(mp::ProxyContext& context) {
    // Check if request was cancelled
    if (context.connection().isCancelled()) {
        throw std::runtime_error("Request cancelled");
    }
    
    // Log the request
    context.connection().log("getAnalytics called");
    
    // Perform operation
    return collectCurrentSnapshot();
}
```

#### Common IPC Pitfalls

**Not handling connection failures:**
```cpp
// WRONG - no error handling
auto result = analytics.getAnalytics();
processResult(result);

// CORRECT - handle potential IPC failure
try {
    auto result = analytics.getAnalytics();
    processResult(result);
} catch (const std::exception& e) {
    std::cerr << "IPC call failed: " << e.what() << "\n";
    // Handle disconnection, retry, or fail gracefully
}
```

**Blocking UI on IPC calls:**
```cpp
// WRONG - blocks UI thread
void onButtonClick() {
    auto result = analytics.getAnalytics();  // Blocks for IPC round-trip
    updateDisplay(result);  // UI frozen during call
}

// CORRECT - use async pattern
void onButtonClick() {
    std::async(std::launch::async, [this]() {
        auto result = analytics.getAnalytics();
        runOnUIThread([this, result]() {
            updateDisplay(result);
        });
    });
}
```

**Forgetting destroy() on process exit:**
```cpp
// WRONG - leaks server-side object
void someFunction() {
    auto analytics = init.makeAnalytics();
    analytics.getAnalytics();
    return;  // Object not destroyed in server!
}

// CORRECT - ensure cleanup
void someFunction() {
    auto analytics = init.makeAnalytics();
    try {
        analytics.getAnalytics();
    } catch (...) {
        analytics.destroy();
        throw;
    }
    analytics.destroy();
}
```

---

### 5. Debugging Methodology

#### Error Categories

**Parse errors** (during schema compilation):
```
example/analytics.capnp:29:47: error: Parse error.
```

Location: Line 29, column 47 of analytics.capnp
Cause: Syntax error, invalid type reference, or import failure
Fix: Check schema syntax at the specified location

**Undefined symbol errors** (during linking):
```
mold: error: undefined symbol: vtable for mp::ProxyServer<AnalyticsInterface>
>>> referenced by init.capnp.proxy-server.c++
```

Cause: Missing object file - the analytics.capnp.proxy-server.c++ wasn't compiled into your executable
Fix: Add `analytics.capnp` to `target_capnp_sources` for this target

**Template instantiation errors** (during compilation):
```
error: no matching function for call to 'CustomReadField'
note: candidate function template not viable: 
      no known conversion from 'TypeList<AnalyticsSnapShot>' 
      to 'TypeList<std::string>'
```

Cause: No CustomReadField defined for AnalyticsSnapShot
Fix: Add CustomReadField specialization in types.h and ensure `$Proxy.includeTypes("types.h")` is in the schema

**Type mismatch errors** (during compilation):
```
error: no type named 'AnalyticsInterface' in namespace 'analytics::capnp'
```

Cause: The schema wasn't included or the namespace doesn't match
Fix: Verify `$Cxx.namespace()` matches your usage and the schema is in `target_capnp_sources`

#### Debugging Strategy

**1. Start with the first error:**

When you see 50 errors, resist the urge to fix everything at once. The first error often cascades into many subsequent errors. Fix the first one, rebuild, and reassess.

**2. Read template errors from the bottom up:**

Template instantiation errors show a stack of template instantiations. The bottom (innermost) instantiation shows the actual problem:

```
error: use of undeclared identifier 'BuildPrimitive'
  236 |     output.set(BuildPrimitive(...));
note: in instantiation of function template specialization 
      'mp::CustomBuildField<AnalyticsSnapShot, ...>' requested here
  203 |         CustomBuildField(TypeList<LocalTypes...>(), ...);
note: in instantiation of function template specialization 
      'mp::BuildField<AnalyticsSnapShot, ...>' requested here
   34 |     BuildField(TypeList<LocalType>(), ...);
```

The actual error is at line 236 - `BuildPrimitive` is undeclared. This is happening because CustomBuildField for AnalyticsSnapShot is missing, causing the fallback to BuildPrimitive which doesn't exist.

**3. Verify includes are working:**

Add a deliberate error to test if your header is included:

```cpp
// In types.h
#error "types.h is being included"

template <typename Value, typename Output>
void CustomBuildField(...) { ... }
```

If you don't see the error during compilation, your `$Proxy.includeTypes("types.h")` isn't working correctly.

**4. Check generated file contents:**

The generated files are in your build directory. You can inspect them:

```bash
# View generated proxy header
less build/example/analytics.capnp.proxy.h

# Check if your types.h is included
grep "types.h" build/example/analytics.capnp.proxy-types.c++
```

This helps verify that the code generator did what you expected.

**5. Use compiler verbose output:**

```bash
cmake --build build --target mpanalytics -- VERBOSE=1
```

This shows the exact compiler commands, include paths, and source files being compiled. Useful for tracking down missing includes or wrong paths.

#### Common Debugging Pitfalls

**Assuming it's a framework bug:**

When something doesn't work, the instinct is to blame the framework. However, libmultiprocess and Cap'n Proto are mature, production-tested systems. 99% of the time, it's a configuration or usage issue.

**Not cleaning the build directory:**

Sometimes stale generated files cause problems:
```bash
rm -rf build
cmake -B build
cmake --build build
```

**Fixing errors without understanding them:**

If you change something and the error goes away, but you don't understand why, you'll hit the same issue again. Take time to understand what each error means.

**Ignoring warnings:**

Warnings often indicate real problems that will cause issues later:
```
warning: unused variable 'value'
```

This might indicate you forgot to actually use the variable in your serialization code.

---

### 6. Advanced Patterns and Best Practices

#### Schema Evolution

Plan for schema evolution from the start:

```capnp
struct MyData $Proxy.wrap("MyData") {
    name @0 :Text;
    age @1 :Int32;
    # Added in v2 - older clients ignore this field
    email @2 :Text;
    # Reserved for future use
    # Never reuse ordinal 3 for a different field
    # futureField @3 :SomeType;
}
```

**Safe changes:**
- Adding new fields with new ordinals
- Adding new methods with new ordinals
- Marking fields as deprecated (document but don't remove)

**Breaking changes:**
- Changing field types
- Removing fields (causes deserialization errors in old clients)
- Changing method signatures

**Best practice:**
- Document the version that added each field
- Use semantic versioning for your IPC interfaces
- Test new versions with old clients

#### Error Handling Patterns

**Exception safety in IPC:**
```cpp
AnalyticsSnapShot AnalyticsImpl::getAnalytics(mp::ProxyContext& context) {
    try {
        return collectSnapshot();
    } catch (const std::exception& e) {
        // Log error with context for debugging
        context.connection().log("getAnalytics failed: " + std::string(e.what()));
        
        // Re-throw or return error state
        // Option 1: Let exception propagate (becomes RPC error)
        throw;
        
        // Option 2: Return empty/error state
        // return AnalyticsSnapShot{};
    }
}
```

**Connection state checking:**
```cpp
void LongRunningOperation(mp::ProxyContext& context) {
    for (int i = 0; i < 1000; ++i) {
        // Check if client disconnected
        if (context.connection().isCancelled()) {
            throw std::runtime_error("Operation cancelled");
        }
        
        // Do work
        processChunk(i);
    }
}
```

#### Testing IPC Code

**Unit testing serialization:**
```cpp
#include <gtest/gtest.h>

TEST(SerializationTest, ProcessInfoRoundTrip) {
    ProcessInfo original;
    original.name = "test_process";
    original.pid = 12345;
    original.cpuPercent = 50.5;
    original.memoryKB = 1024;
    
    // Serialize to Cap'n Proto message
    capnp::MallocMessageBuilder message;
    auto builder = message.initRoot<analytics::capnp::ProcessInfo>();
    
    mp::InvokeContext ctx;
    CustomBuildField(mp::TypeList<ProcessInfo>(), mp::Priority<1>(),
                     ctx, original, builder);
    
    // Deserialize back
    ProcessInfo deserialized;
    auto reader = builder.asReader();
    CustomReadField(mp::TypeList<ProcessInfo>(), mp::Priority<1>(),
                   ctx, reader, mp::ReadDestUpdate(deserialized));
    
    // Verify
    EXPECT_EQ(original.name, deserialized.name);
    EXPECT_EQ(original.pid, deserialized.pid);
    EXPECT_DOUBLE_EQ(original.cpuPercent, deserialized.cpuPercent);
    EXPECT_EQ(original.memoryKB, deserialized.memoryKB);
}
```

**Integration testing with real processes:**
```cpp
TEST(IPCTest, AnalyticsEndToEnd) {
    // 1. Create socketpair
    int fds[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    
    // 2. Fork child process
    pid_t pid = fork();
    ASSERT_NE(-1, pid);
    
    if (pid == 0) {
        // Child: run analytics server
        close(fds[0]);
        
        mp::EventLoop loop("test", LogFunction);
        AnalyticsImpl analytics;
        mp::ServeStream<AnalyticsInterface>(loop, fds[1], analytics);
        loop.loop();
        
        exit(0);
    } else {
        // Parent: test client calls
        close(fds[1]);
        
        mp::Connection conn{fds[0]};
        auto client = conn.makeClient<AnalyticsInterface>();
        
        auto result = client.getAnalytics();
        
        EXPECT_FALSE(result.timestamp.empty());
        EXPECT_GE(result.cpuUsagePercent, 0.0);
        
        client.destroy();
        
        // Wait for child
        int status;
        waitpid(pid, &status, 0);
        EXPECT_EQ(0, WEXITSTATUS(status));
    }
}
```

#### Performance Considerations

**Zero-copy deserialization:**

Cap'n Proto's main advantage is zero-copy reads. The deserialized data points directly into the received message buffer. This means:

```cpp
// No copying happens here!
auto snapshot = analytics.getAnalytics();
auto name = snapshot.topProcesses[0].name;  // Points into message buffer
```

However, this also means the data is only valid while the message buffer exists. If you need to store the data:

```cpp
// Copy to owned storage
std::vector<ProcessInfo> processes;
for (const auto& proc : snapshot.topProcesses) {
    processes.push_back(ProcessInfo{
        std::string(proc.name),  // Copy string
        proc.pid,
        proc.cpuPercent,
        proc.memoryKB
    });
}
```

**Minimize round-trips:**

Each IPC call has overhead. Design interfaces to reduce round-trips:

```cpp
// BAD - multiple round-trips
for (int i = 0; i < 100; ++i) {
    auto metric = analytics.getMetric(i);
    processMetric(metric);
}

// GOOD - single round-trip
auto metrics = analytics.getAllMetrics();
for (const auto& metric : metrics) {
    processMetric(metric);
}
```

**Batch operations:**

```capnp
interface AnalyticsInterface {
    # Instead of individual getters
    # getMetric @0 (id: Int32) -> (metric: Metric);
    
    # Provide batch operation
    getMetrics @0 (ids: List(Int32)) -> (metrics: List(Metric));
}
```

#### Security Considerations

**Input validation:**

Even though data comes from "your" other process, validate it:

```cpp
AnalyticsSnapShot AnalyticsImpl::getAnalytics(mp::ProxyContext& context) {
    auto snapshot = collectSnapshot();
    
    // Validate before returning
    if (snapshot.processCount < 0) {
        throw std::invalid_argument("Invalid process count");
    }
    
    if (snapshot.cpuUsagePercent < 0.0 || snapshot.cpuUsagePercent > 100.0) {
        throw std::invalid_argument("Invalid CPU percentage");
    }
    
    return snapshot;
}
```

**Privilege separation:**

Run different processes with different privileges:

```bash
# Node process runs as root (needs network access)
sudo ./mpnode

# Wallet process runs as user (limited privileges)
./mpwallet

# GUI runs as user (no elevated privileges)
./mpgui
```

If the wallet or GUI is compromised, attackers can't access node internals due to privilege boundaries.

**Rate limiting:**

Prevent one process from overwhelming another:

```cpp
class RateLimitedAnalytics {
    std::atomic<int> requestCount{0};
    std::chrono::steady_clock::time_point lastReset;
    
public:
    AnalyticsSnapShot getAnalytics(mp::ProxyContext& context) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastReset > std::chrono::seconds(1)) {
            requestCount = 0;
            lastReset = now;
        }
        
        if (++requestCount > 100) {
            throw std::runtime_error("Rate limit exceeded");
        }
        
        return collectSnapshot();
    }
};
```

---

## Analytics Monitor Example

### Complete Implementation

This section walks through building a complete system analytics monitor with IPC.

#### Project Structure

```
libmultiprocess/example/
├── analytics_snapshot.h    # C++ data structures
├── analytics.h             # Analytics interface definition
├── analytics.cpp           # Analytics server implementation
├── analytics.capnp         # Cap'n Proto schema
├── types.h                 # Serialization hooks
├── init.capnp              # Init interface
└── CMakeLists.txt          # Build configuration
```

#### Step 1: Define C++ Data Structures

**analytics_snapshot.h:**
```cpp
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
```

#### Step 2: Define Cap'n Proto Schema

**analytics.capnp:**
```capnp
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
```

#### Step 3: Implement Serialization Hooks

**types.h:**
```cpp
#ifndef EXAMPLE_TYPES_H
#define EXAMPLE_TYPES_H

#include "analytics_snapshot.h"
#include <mp/proxy-types.h>
#include <mp/type-string.h>
#include <mp/type-vector.h>

namespace mp {

// Serialize ProcessInfo
template <typename Value, typename Output>
void CustomBuildField(TypeList<ProcessInfo>, Priority<1>, 
                      InvokeContext& invoke_context,
                      Value&& value, Output&& output)
{
    auto builder = output.init();
    builder.setName(value.name);
    builder.setPid(value.pid);
    builder.setCpuPercent(value.cpuPercent);
    builder.setMemoryKB(value.memoryKB);
}

// Deserialize ProcessInfo
template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<ProcessInfo>, Priority<1>,
                               InvokeContext& invoke_context,
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

// Serialize AnalyticsSnapShot
template <typename Value, typename Output>
void CustomBuildField(TypeList<AnalyticsSnapShot>, Priority<1>,
                      InvokeContext& invoke_context,
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
    
    auto processes = builder.initTopProcesses(value.topProcesses.size());
    for (size_t i = 0; i < value.topProcesses.size(); ++i) {
        auto proc = processes[i];
        proc.setName(value.topProcesses[i].name);
        proc.setPid(value.topProcesses[i].pid);
        proc.setCpuPercent(value.topProcesses[i].cpuPercent);
        proc.setMemoryKB(value.topProcesses[i].memoryKB);
    }
}

// Deserialize AnalyticsSnapShot
template <typename Input, typename ReadDest>
decltype(auto) CustomReadField(TypeList<AnalyticsSnapShot>, Priority<1>,
                               InvokeContext& invoke_context,
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

} // namespace mp

#endif // EXAMPLE_TYPES_H
```

#### Step 4: Implement Analytics Interface

**analytics.h:**
```cpp
#ifndef EXAMPLE_ANALYTICS_H
#define EXAMPLE_ANALYTICS_H

#include "analytics_snapshot.h"

class Analytics {
public:
    virtual ~Analytics() = default;
    virtual AnalyticsSnapShot getAnalytics() = 0;
};

#endif // EXAMPLE_ANALYTICS_H
```

**analytics.cpp:**
```cpp
#include "analytics.h"
#include "init.h"
#include <mp/proxy.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <dirent.h>
#include <algorithm>

class AnalyticsImpl : public Analytics {
private:
    unsigned long long lastTotalUser{0};
    unsigned long long lastTotalUserLow{0};
    unsigned long long lastTotalSys{0};
    unsigned long long lastTotalIdle{0};
    bool initialized{false};
    
    void initCPUTracking() {
        std::ifstream file("/proc/stat");
        std::string cpu;
        file >> cpu >> lastTotalUser >> lastTotalUserLow 
             >> lastTotalSys >> lastTotalIdle;
        initialized = true;
    }
    
    double getCPUUsage() {
        if (!initialized) {
            initCPUTracking();
            return 0.0;
        }
        
        std::ifstream file("/proc/stat");
        std::string cpu;
        unsigned long long totalUser, totalUserLow, totalSys, totalIdle;
        
        file >> cpu >> totalUser >> totalUserLow >> totalSys >> totalIdle;
        
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
    
    std::string getCurrentTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }

public:
    AnalyticsSnapShot getAnalytics() override {
        AnalyticsSnapShot snapshot;
        snapshot.timestamp = getCurrentTimestamp();
        snapshot.cpuUsagePercent = getCPUUsage();
        
        getMemoryInfo(snapshot.totalMemoryMB, snapshot.availableMemoryMB,
                     snapshot.cachedMemoryMB, snapshot.buffersMemoryMB);
        snapshot.usedMemoryMB = snapshot.totalMemoryMB - 
                               snapshot.availableMemoryMB - 
                               snapshot.cachedMemoryMB - 
                               snapshot.buffersMemoryMB;
        
        snapshot.topProcesses = getTopProcesses(10);
        snapshot.processCount = static_cast<int>(snapshot.topProcesses.size());
        
        return snapshot;
    }
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <fd>\n";
        return 1;
    }
    
    int fd;
    if (std::from_chars(argv[1], argv[1] + strlen(argv[1]), fd).ec != std::errc{}) {
        std::cerr << argv[1] << " is not a valid file descriptor\n";
        return 1;
    }
    
    mp::EventLoop loop("mpanalytics", [](const char* msg) {
        std::cout << msg << std::endl;
    });
    
    std::unique_ptr<Init> init = std::make_unique<InitImpl>();
    mp::ServeStream<InitInterface>(loop, fd, *init);
    loop.loop();
    
    return 0;
}
```

#### Step 5: Configure Build System

**CMakeLists.txt:**
```cmake
include(${PROJECT_SOURCE_DIR}/cmake/TargetCapnpSources.cmake)

add_executable(mpanalytics
  analytics.cpp
)
target_capnp_sources(mpanalytics ${CMAKE_CURRENT_SOURCE_DIR} 
                     init.capnp 
                     analytics.capnp 
                     calculator.capnp 
                     printer.capnp)
target_include_directories(mpanalytics PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mpanalytics PRIVATE Threads::Threads)
```

#### Step 6: Build and Run

```bash
# Configure
cmake -B build

# Build
cmake --build build --target mpanalytics

# Run (fd will be provided by parent process)
./build/example/mpanalytics 3
```

### Workflow Explanation

1. **Parent process** creates socketpair and spawns `mpanalytics` with fd
2. **Child process** (mpanalytics) reads fd from argv[1]
3. **Event loop** starts listening on the fd for RPC requests
4. **Client** calls `getAnalytics()` through proxy
5. **Proxy client** serializes request to Cap'n Proto message
6. **Message sent** over fd to analytics process
7. **Proxy server** deserializes and calls `AnalyticsImpl::getAnalytics()`
8. **Implementation** collects system metrics from `/proc`
9. **Response serialized** using CustomBuildField hooks
10. **Message sent back** to client
11. **Proxy client** deserializes using CustomReadField hooks
12. **Client receives** native C++ `AnalyticsSnapShot` object

---

## Common Pitfalls Reference

### Quick Troubleshooting Guide

| Error | Cause | Fix |
|-------|-------|-----|
| `Parse error` at line X | Schema syntax error | Check syntax at specified line |
| `Import failed: mp/proxy.capnp` | Missing leading slash | Use `/mp/proxy.capnp` |
| `undefined symbol: vtable for ProxyServer<X>` | Missing schema in CMake | Add schema to `target_capnp_sources` |
| `no matching function for CustomReadField` | Missing serialization hook | Add CustomReadField in types.h |
| `duplicate symbol: main` | Multiple main() functions | One executable per main() |
| `use of undeclared identifier BuildPrimitive` | Missing custom serialization | Add CustomBuildField/CustomReadField |
| `Not defined: Proxy` | Schema import error | Check import paths and includes |

### Don't Do This

**1. Forget to check has():**
```cpp
// CRASH RISK
auto reader = input.get();
```

**2. Link multiple main() functions:**
```cmake
# LINKER ERROR
add_executable(bad
  analytics.cpp    # Has main()
  calculator.cpp   # Also has main()
)
```

**3. Assume structs serialize automatically:**
```cpp
// WON'T COMPILE
struct MyData { int x; };
// No CustomBuildField defined!
```

**4. Forget destroy():**
```cpp
// MEMORY LEAK IN SERVER
auto obj = factory.createObject();
obj.doWork();
// Forgot obj.destroy()!
```

**5. Block UI on IPC:**
```cpp
// UI FREEZE
auto data = remote.getData();  // Blocks for network round-trip
updateDisplay(data);
```

---

## Best Practices

### Schema Design

1. **Plan for evolution** - Use ordinals that allow adding fields
2. **Document versions** - Comment when fields were added
3. **Group related operations** - Don't create too many interfaces
4. **Keep interfaces cohesive** - Related functionality in one interface
5. **Use destroy() for all objects** - Prevent server-side leaks

### Serialization

1. **Always check has()** - Prevent crashes on missing fields
2. **Handle errors gracefully** - Try-catch in serialization code
3. **Test round-trips** - Verify serialize→deserialize works
4. **Document custom types** - Explain serialization strategy
5. **Keep it simple** - Don't over-optimize prematurely

### Build System

1. **Include all schemas** - Add every referenced schema to CMake
2. **One main per executable** - Separate process types
3. **Clean builds when stuck** - `rm -rf build` fixes stale files
4. **Use verbose builds** - `VERBOSE=1` to debug compiler issues
5. **Check generated files** - Verify code gen did what you expected

### Runtime

1. **Handle disconnection** - IPC calls can fail
2. **Use async patterns** - Don't block UI threads
3. **Implement timeouts** - Don't wait forever for responses
4. **Log errors with context** - Include connection info in logs
5. **Test process crashes** - Verify graceful failure handling

### Security

1. **Validate all inputs** - Even from "trusted" processes
2. **Use privilege separation** - Different privileges per process
3. **Implement rate limiting** - Prevent resource exhaustion
4. **Audit IPC boundaries** - Review what crosses processes
5. **Plan for compromise** - Assume processes can be attacked

---

## Conclusion

Building IPC systems requires attention to multiple layers: schema design, type serialization, build configuration, process management, and error handling. The Bitcoin Core approach demonstrates that proper IPC provides real security and stability benefits through process isolation.

Key takeaways:
- **Understand the full stack** - Schema, serialization, build, runtime
- **Start simple** - Get basic types working before complexity
- **Test thoroughly** - Unit tests for serialization, integration for IPC
- **Plan for evolution** - Schema versioning from day one
- **Handle errors** - IPC can fail in ways single-process code cannot

The upfront complexity pays dividends in production through better isolation, security, and maintainability.
