# Sub-spec 02 — `MockSMSServer` (UDS transport + threading + packet builders)

**Primary spec:** [`overview-spec.md`](overview-spec.md).
**Commit:** 2 of 6. Creates the in-process mock SMS server used by every
test in the new integration suite. After this commit the build still
compiles (helper is registered in `TESTHELPER_SRCS` but not yet
referenced by any test header).

---

## 0. Agent execution instructions (must obey)

1. Base on branch `EWM15431_live-listener-interface`; **do not** rebase
   onto `main` / `master`.
2. **Scope fence.** Touch only the three files in §2 below.
   Do **not** modify any file under `Framework/LiveData/src/` or
   `Framework/LiveData/inc/`.
3. **Static verification only — DO NOT build, DO NOT run tests.**
   Verify by reading the resulting source, cross-checking the Poco
   socket API references in §3 against the cited line ranges in
   `SNSLiveEventDataListener.cpp`, and confirming that all packet-builder
   helpers `#include "ADARAPackets.h"` rather than duplicating byte
   arrays.
4. **No build artefacts in the PR.**
5. **No production code changes.** If you believe the listener must
   change to make the transport work, you have misread §3.4 — stop and
   ask.
6. **Ambiguity protocol.** If the existing `ADARAPackets.h` fixture
   inventory in §4 does not contain an array required by a builder you
   are writing, **stop** and surface it in the PR description rather
   than hand-rolling a parallel byte layout.
7. **Real integration test.** This helper drives a real Poco
   `Poco::Net::ServerSocket` over a real UDS path. Do not mock the
   socket. Do not bypass `Poco`.
8. **One commit.** No edits to `SNSLiveEventDataListenerTest.h` (which
   does not yet exist) or to `Framework/LiveData/CMakeLists.txt`. Those
   come in `subspec03`.

---

## 1. Goal of this commit

Create the test helper that every subsequent test in the integration
suite will use to drive the real `SNSLiveEventDataListener` over a Unix
domain socket. The helper:

- binds a `Poco::Net::ServerSocket` to a UDS path,
- accepts exactly one client connection on a background thread,
- plays back a scripted sequence of ADARA packets (preferring verbatim
  fixtures from `ADARAPackets.h`, or applying small patches via
  `Testing::buildXxxPkt()` helpers),
- supports `PktWaitForExtract{}` gates so tests can deterministically
  interleave `extractData()` with packet delivery,
- self-watchdogs to avoid hanging a `ctest` invocation.

It is registered in `TESTHELPER_SRCS` of
`Framework/LiveData/test/CMakeLists.txt` (the precedent is
`KafkaTesting.h` + `TestDataListener.cpp`), so it compiles into the
existing `LiveDataTest` binary.

---

## 2. Files touched in this commit

| Action | Path |
|---|---|
| Create | `Framework/LiveData/test/MockSMSServer.h` |
| Create | `Framework/LiveData/test/MockSMSServer.cpp` |
| Edit | `Framework/LiveData/test/CMakeLists.txt` — add `MockSMSServer.cpp` to `TESTHELPER_SRCS` |

The `LiveDataTest` CMake target already exists (see
`Framework/LiveData/test/CMakeLists.txt:12`); no new test executable is
introduced.

---

## 3. Transport: Unix-domain socket

### 3.1 Rationale

- TCP loopback (`127.0.0.1`) is blocked or filtered on several Mantid
  CI runners — historically the reason the legacy suite was disabled.
- The SNS production deployment is Linux-only.
- The listener already supports UDS through its
  `Poco::Net::SocketAddress` argument
  (`SNSLiveEventDataListener.cpp:141-156`); no production change is
  required.

### 3.2 Platform guard

Wrap the entire body of both `MockSMSServer.h` and `MockSMSServer.cpp`
(after `#pragma once`) in:

```cpp
#ifndef _WIN32
  // ... entire content ...
#endif
```

There is intentionally no Windows stub in this file — the Windows stub
is in `SNSLiveEventDataListenerTest.h` (see `subspec03`). On Windows,
`MockSMSServer.cpp` compiles to an empty translation unit.

Precedent: search `Framework/*/test/` for `#ifndef _WIN32` for the
existing skipped-on-Windows convention.

### 3.3 Socket-path lifetime

The UDS path comes from `Poco::TemporaryFile` (framework-wide
convention; see `SaveGSSTest.h`, `SaveGDATest.h`,
`SaveOpenGenieAsciiTest.h`, `DownloadFileTest.h`,
`InternetHelperTest.h`). The path lives in the **test fixture**, not
inside `MockSMSServer`, because the fixture must outlive the server.

The `MockSMSServer` constructor takes the path as a `std::string`
parameter. The destructor calls `::unlink(m_path.c_str())` (not
`std::filesystem::remove`) so that on Linux the socket file is removed
even if `accept()` never completed.

### 3.4 Correct Poco UDS `SocketAddress` construction

> **IMPORTANT — the `"unix:path"` string syntax is not valid Poco.**
>
> Poco's `SocketAddress` string constructor parses `"host:port"` for
> TCP. It does **not** recognise a `"unix:/path"` or `"unix:path"`
> prefix. Attempting `Poco::Net::SocketAddress("unix:/tmp/sock")` will
> throw or misinterpret the string as a hostname.

The correct constructor for a Unix-domain socket address is the
two-argument form that accepts an `AddressFamily` enum:

```cpp
Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, path)
```

#### Server side (this commit)

In `MockSMSServer::start()`:

```cpp
m_listenSocket = Poco::Net::ServerSocket(
    Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, m_path),
    /*backlog=*/1);
```

#### Client side (for awareness — not coded in this commit)

The test fixture (created in `subspec03`) will call
`m_listener->connect(udsAddr)` where:

```cpp
Poco::Net::SocketAddress udsAddr(
    Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath);
```

The listener's `connect(const Poco::Net::SocketAddress&)` at
`SNSLiveEventDataListener.cpp:141-156` already handles UDS via the enum
constructor above. Do **not** patch `connect()` to accept a string
path — that would be a production code change.

### 3.5 Connection-sequence reminder

The fixture (in `subspec03`) will:

1. queue the script,
2. call `m_server->start()` — server binds and begins `accept()`,
3. construct the listener,
4. call `m_listener->connect(udsAddr)` (blocking until the server
   accepts),
5. call `m_listener->start(startTime)`,
6. drive the scenario.

`MockSMSServer::start()` must therefore return promptly (it spawns the
background thread immediately; `accept()` happens on that thread).

---

## 4. `MockSMSServer.h` — public surface

**Two files, not header-only.** Header-only would (a) recompile the Poco
socket plumbing and packet builders into every test translation unit,
and (b) risk ODR collisions with the file-scope `const unsigned char`
exemplars in `ADARAPackets.h`.

Required content (typeset, not necessarily verbatim — the structural
elements below are mandatory):

```cpp
#pragma once
#ifndef _WIN32

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace Mantid::LiveData::Testing {

// One step in the server's playback script.
struct PktGarbage       { std::vector<uint8_t> bytes; };
struct PktDisconnect    {};
struct PktWaitForExtract{};  // gate: blocks until the test signals
                             // (used to deterministically interleave
                             //  extractData() with packet delivery).

using ScriptEntry = std::variant<
    std::vector<uint8_t>,    // raw packet bytes (preferred — built by
                             // the helpers in §5 of this sub-spec)
    PktGarbage,
    PktDisconnect,
    PktWaitForExtract>;

class MockSMSServer {
public:
    // path: absolute UDS path (must not exist when start() is called).
    explicit MockSMSServer(std::string path);
    ~MockSMSServer();        // joins server thread; closes sockets; unlinks path.

    MockSMSServer(const MockSMSServer&) = delete;
    MockSMSServer& operator=(const MockSMSServer&) = delete;

    // Begin listening.  Returns immediately; accept() happens on bg thread.
    // Must be called BEFORE the listener calls connect().
    void start();

    // Append script entries.  ALL entries must be queued before the
    // listener's background thread begins reading (i.e. before start()).
    void script(std::initializer_list<ScriptEntry> entries);
    void scriptAppend(ScriptEntry entry);

    // Release the next PktWaitForExtract gate.  Called by the test
    // fixture immediately after extractData() returns.
    void releaseExtractGate();

    // Diagnostics for assertions.
    bool        clientConnected() const;
    std::size_t bytesSent()       const;
    std::size_t scriptIndex()     const;  // how many entries have been delivered

    // Self-watchdog deadline (default 30 s).  If the script is not
    // exhausted by then, the server closes its sockets so the client
    // observes EOF rather than hanging.
    void setWatchdog(std::chrono::seconds);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// RAII watchdog: if not disarmed within the deadline, calls
/// g_log.fatal and std::abort().  Arm at the top of every test that
/// drives the listener; disarmed by fixture tearDown.
class TestWatchdog {
public:
    explicit TestWatchdog(std::chrono::seconds deadline,
                          std::string testName);
    ~TestWatchdog();   // disarms if still armed
    void disarm();
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ----- Packet-builder helpers; see §5 of this sub-spec. -----

std::vector<uint8_t> buildHeartbeatPkt(uint64_t pulseId);
std::vector<uint8_t> buildRunStatusPkt(/* see §5 */);
std::vector<uint8_t> buildVariableU32Pkt(uint32_t devId, uint32_t pvId,
                                         uint32_t value, uint64_t pulseId);
std::vector<uint8_t> buildVariableDoublePkt(uint32_t devId, uint32_t pvId,
                                            double value, uint64_t pulseId);
std::vector<uint8_t> buildPausePkt();   // verbatim copy of AnnotationPacketType3
std::vector<uint8_t> buildResumePkt();  // verbatim copy of AnnotationPacketType4
std::vector<uint8_t> buildBankedEventPkt(/* see §5 */);
std::vector<uint8_t> buildBeamMonitorPkt(/* see §5 */);
std::vector<uint8_t> buildRunInfoPkt(const std::string& proposalId,
                                     const std::string& title);
std::vector<uint8_t> buildGeometryPkt(const std::string& xml);
std::vector<uint8_t> buildBeamlineInfoPkt(const std::string& longName);
std::vector<uint8_t> buildDeviceDescriptorPkt(uint32_t devId,
                                              const std::string& xmlDescriptor);

} // namespace Mantid::LiveData::Testing

#endif // !_WIN32
```

`MockSMSServer.h` must only include lightweight headers — **no
`Poco::Net::*` headers in the public surface**. The socket members live
inside the `Impl` pimpl in `.cpp`.

### 4.1 Lifetime / threading rules

- **Constructor** only initialises members; it does **not** bind or
  listen.
- **`start()`** creates the `Poco::Net::ServerSocket` bound to
  `Poco::Net::SocketAddress(Poco::Net::AddressFamily::UNIX_LOCAL, m_path)`,
  sets backlog=1, then launches the background thread. The background
  thread calls `accept()` (blocking) for exactly one client connection,
  then drains the script sequentially.
- **Destructor** sets a stop flag, shuts down the sockets (to break any
  blocked `accept` or `send`), joins the thread with a bounded timeout
  (5 s); if the join fails, calls `g_log.fatal` and `std::abort()` — a
  hung test process is worse than a crashed one.
- The destructor calls `::unlink(m_path.c_str())` so that on Linux the
  socket file is removed even if `accept()` never completed.

### 4.2 Driving modes

The server thread iterates the script vector. For each entry:

- `std::vector<uint8_t>` → `m_clientSocket.sendBytes(data.data(),
  static_cast<int>(data.size()))`. Advance `m_bytesSent` and
  `m_scriptIndex`.
- `PktGarbage` → send the arbitrary bytes (used to verify
  `ADARA::invalid_packet` propagation — see `subspec06` §6.9).
- `PktDisconnect` → `m_clientSocket.close()`; mark "client gone"; stop.
- `PktWaitForExtract` → acquire the condition-variable mutex and wait
  on the CV until `releaseExtractGate()` is called or the watchdog
  fires; then continue with the next script entry.

No `Poco::Thread::sleep` between entries by default. Inter-entry delays
are explicitly out of scope for v1.

### 4.3 Thread-safety contract

- `script()` / `scriptAppend()` are only safe to call **before**
  `start()`. The script vector is read-only once the background thread
  is running.
- `releaseExtractGate()` is safe to call from the foreground test
  thread at any time after `start()`.
- `bytesSent()` / `scriptIndex()` / `clientConnected()` are protected
  by a `std::mutex`.

### 4.4 `MockSMSServer.cpp` include requirements

```cpp
#include "MockSMSServer.h"
#include "ADARAPackets.h"            // binary exemplar arrays (§5)
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/NetException.h>
#include <Poco/Thread.h>
#include "MantidKernel/Logger.h"     // g_log.fatal for abort path
#include <sys/socket.h>
#include <unistd.h>                  // ::unlink
```

### 4.5 Hang protection — three layers in this commit

> **Notes for reviewer:** *[ agent should not run `ctest`! ]*

A test that hangs at runtime blocks the entire `ctest` invocation. This
commit ships three of four defence layers; the fourth
(`extractWithTimeout` + `waitFor`) lives in
`SNSLiveEventDataListenerTest.h` and ships in `subspec03`.

1. **`MockSMSServer` self-watchdog** — if the script is not exhausted
   within `setWatchdog()` seconds (default 30 s), the server closes its
   client socket so the listener observes EOF and surfaces the failure
   via its normal exception path
   (`SNSLiveEventDataListener.cpp:274-278, 336-342`).
2. **Bounded join in destructor** — 5 s; failure → `g_log.fatal` +
   `std::abort()`.
3. **`TestWatchdog` RAII helper** — constructed at the top of every
   test that drives the listener; arms a background thread that, if not
   disarmed within 60 s, calls `g_log.fatal` and `std::abort()`.
   Disarmed by the fixture's `tearDown`.

These limits are deliberately loose (5 / 30 / 60 s) — they exist to
catch *bugs*, not to enforce performance. Healthy tests complete in
< 1 s.

---

## 5. Packet builders — canonical fixture reuse

### 5.1 Guiding principle

`Framework/LiveData/test/ADARAPackets.h` contains a curated set of
pre-built, validated packet byte arrays and their full byte-layout
diagrams in header comments. These arrays are consumed by
`Framework/LiveData/test/ADARAPacketTest.h` and have been round-tripped
through the real `ADARA::Parser`. **They are the canonical fixtures for
this codebase.**

**Default rule:** `MockSMSServer.cpp` must `#include "ADARAPackets.h"`
and emit those arrays verbatim wherever a fixture exists for the needed
packet type. Do not re-derive byte layouts. Do not maintain parallel
copies of payloads.

**Exception rule:** Hand-rolled builders are permitted **only** for
packet types whose payload must vary per call in ways that cannot be
expressed as a small fixed-field patch to an existing fixture. Each
such builder must cite the corresponding struct in
`Framework/LiveData/src/ADARAPackets.cpp` by line range, with a
one-sentence justification.

### 5.2 Fixture inventory

Every packet type needed by the integration scenarios has a pre-built
fixture in `ADARAPackets.h`. There are **no gaps** requiring
hand-rolled builders from scratch:

| Packet type | `ADARAPackets.h` array(s) | Notes |
|---|---|---|
| Banked Event v0 | `bankedEventPacketV0[96]` | Fixed events |
| Banked Event v1 | `bankedEventPacketV1[216]` | Full byte-layout diagram in header |
| Beam Monitor v0 | `beamMonitorPacketV0[32]` | |
| Beam Monitor v1 | `beamMonitorPacketV1[136]` | |
| RTDL v0 | `rtdlPacketV0[136]` | |
| RTDL v1 | `rtdlPacketV1[136]` | |
| Pixel Map Alt v0 | `pixelMappingAltPktV0[48]` | |
| Pixel Map Alt v1 | `pixelMappingAltPktV1direct[48]`, `pixelMappingAltPktV1shorthand[44]` | |
| Run Status v0 | `runStatusPacketV0[28]` | |
| Run Status v1 — no run | `runStatusPacketV1NoRun[36]` | Byte-layout comments at byte 16 |
| Run Status v1 — new run | `runStatusPacketV1NewRun[36]` | Byte-layout comments at byte 16 |
| Run Status v1 — EOF | `runStatusPacketV1RunEOF[36]` | |
| Run Status v1 — BOF | `runStatusPacketV1RunBOF[36]` | |
| Run Status v1 — end run | `runStatusPacketV1EndRun[36]` | |
| Run Info v0 | `runInfoPacketV0[92]` | Contains `"<root>Power to Run Info!</root>"` |
| Translation Complete v0 | `translationCompletePacketV0[48]` | |
| Client Hello v0 | `clientHelloPacketV0[20]` | |
| Client Hello v1 | `clientHelloPacketV1[24]` | |
| Heartbeat v0 | `heartbeatPacketV0[16]` | Zero-payload; only header timestamp varies |
| Geometry v0 | `geometryPacketV0[92]` | Contains VACUO instrument XML |
| Beamline Info v0 | `beamlineInfoPacketV0[32]` | Contains `"42CG3BIOSANS"` |
| Beamline Info v1 | `beamlineInfoPacketV1[32]` | |
| Detector Bank Set v0 | `detectorBankSetPacketV0[92]` | |
| Data Done v0 | `dataDonePacketV0[16]` | |
| Sync v0 | `syncPacket[44]` | |
| Annotation type 0 (Generic) | `AnnotationPacketType0[44]` | `'Run 44635 Started.'` |
| Annotation type 1 (Scan Start) | `AnnotationPacketType1[52]` | |
| Annotation type 2 (Scan Stop) | `AnnotationPacketType2[52]` | |
| Annotation type 3 (Pause) | `AnnotationPacketType3[44]` | `'Run 44635 Paused.'` |
| Annotation type 4 (Resume) | `AnnotationPacketType4[44]` | `'Run 44635 Resumed.'` |
| Annotation type 5 (Comment) | `AnnotationPacketType5[32]` | |
| Variable Value U32 v0 | `variableU32Packet[32]` | |
| Variable Value double v0 | `variableDoublePacket[36]` | |
| Variable Value string v0 | `variableStringPacketValue1[36]` | `'N/A'` |
| Device Descriptor v0 | `devDesPacket[2600]` | Large XML; good for exact-fixture reuse |

### 5.3 Builder API

Non-member helpers in the `Mantid::LiveData::Testing` namespace return
`std::vector<uint8_t>`. For types where the fixture is usable verbatim
(or with small field patches), the builder copies the fixture array and
patches the specified bytes in-place. For types where the payload size
varies, the builder assembles a fresh packet from header + variable
section.

Builders must `#include "ADARAPackets.h"`. **They must not duplicate the
byte arrays inline.**

#### Verbatim-with-patch builders

**`buildHeartbeatPkt(uint64_t pulseId)`** — base
`heartbeatPacketV0[16]`. Heartbeat has no payload — only the 16-byte
ADARA header. Header layout (LE):
```
Byte  0–3:  payload length    (0x00000000 for heartbeat)
Byte  4–7:  packet type/ver   (0x00400900 for heartbeat v0)
Byte  8–11: pulse ID seconds  (upper 32 bits of pulseId >> 32)
Byte 12–15: pulse ID nanos    (lower 32 bits of pulseId & 0xFFFFFFFF)
```

**`buildRunStatusPkt(ADARA::RunStatus::Enum status, uint32_t runNumber,
uint64_t pulseId)`** — select base fixture by `status`:

- `NEW_RUN`   → `runStatusPacketV1NewRun[36]`
- `END_RUN`   → `runStatusPacketV1EndRun[36]`
- `STATE`     → `runStatusPacketV1NoRun[36]`
- `NEW_FILE`  → `runStatusPacketV1RunBOF[36]`
- `EOF_PKT`   → `runStatusPacketV1RunEOF[36]`

Run Status v1 byte layout (from `ADARAPackets.h` comments at line ~201):
```
Byte  0–3:  payload length   = 0x00000014 (20 bytes)
Byte  4–7:  type/ver          = 0x00400301 (RUN_STATUS v1)
Byte  8–11: timestamp seconds (patch with pulseId >> 32)
Byte 12–15: timestamp nanos   (patch with pulseId & 0xFFFFFFFF)
Byte 16–19: run number        (patch with runNumber, LE u32)
Byte 20–23: run start seconds (leave as-is from fixture)
Byte 24–27: status | flags    (patch status nibble; OR in caller value)
Byte 28–31: paused            (leave as-is from fixture)
Byte 32–35: addendum          (leave as-is from fixture)
```

Patch bytes 8–15 with the caller-supplied `pulseId`, 16–19 with
`runNumber`. Patch bytes 24–27: keep the flags nibble from the fixture
and OR in the caller-supplied status value.

*Justification for patching (not verbatim):* run number and pulse
timestamp must be caller-controlled for scenarios in `subspec04`/`05`.

**`buildVariableU32Pkt(uint32_t devId, uint32_t pvId, uint32_t value,
uint64_t pulseId)`** — base `variableU32Packet[32]`:
```
Byte  0–3:  payload length   = 0x00000010 (16 bytes)
Byte  4–7:  type/ver          = 0x00800100
Byte  8–11: timestamp seconds (patch with pulseId >> 32)
Byte 12–15: timestamp nanos   (patch with pulseId & 0xFFFFFFFF)
Byte 16–19: device ID         (patch with devId)
Byte 20–23: variable ID       (patch with pvId)
Byte 24–27: status/severity   (leave as-is: 0x00000000 = OK/OK)
Byte 28–31: value             (patch with value)
```

**`buildVariableDoublePkt(uint32_t devId, uint32_t pvId, double value,
uint64_t pulseId)`** — base `variableDoublePacket[36]`:
```
Byte  0–3:  payload length   = 0x00000014 (20 bytes)
Byte  4–7:  type/ver          = 0x00800200
Byte  8–11: timestamp seconds (patch with pulseId >> 32)
Byte 12–15: timestamp nanos   (patch with pulseId & 0xFFFFFFFF)
Byte 16–19: device ID         (patch with devId)
Byte 20–23: variable ID       (patch with pvId)
Byte 24–27: status/severity   (leave as-is: 0x00000000)
Byte 28–35: value             (patch with double via
                               memcpy(&pkt[28], &value, 8) — IEEE 754 LE)
```

**`buildPausePkt()`** — verbatim copy of `AnnotationPacketType3`:
```cpp
return std::vector<uint8_t>(
    AnnotationPacketType3,
    AnnotationPacketType3 + sizeof(AnnotationPacketType3));
```

**`buildResumePkt()`** — verbatim copy of `AnnotationPacketType4` (same
pattern as above).

For scan-start / scan-stop / generic annotations, callers use the
`AnnotationPacketType1` / `AnnotationPacketType2` /
`AnnotationPacketType0` arrays directly via the `PKT(name)` shorthand
defined in `subspec03`. No additional builder is required.

#### Variable-length builders

These must assemble a fresh packet because the payload size varies. They
use `ADARAPackets.h` only as a **reference for the header structure**
(cite the matching fixture).

**`buildBankedEventPkt(uint64_t pulseId, double pulseChargePc,
std::span<const PixelTof> events)`** — reference
`bankedEventPacketV1[216]` (layout diagram in `ADARAPackets.h` at line
~31).

*Justification:* the events section (bank ID + event array) has
variable length depending on `events.size()`.

Header + fixed-section layout to replicate:
```
Byte  0–3:  payload length (compute from event count)
Byte  4–7:  type/ver = 0x00400001 (BANKED_EVENT v1)
Byte  8–11: pulse ID seconds
Byte 12–15: pulse ID nanos
Byte 16–19: pulse charge (units of 10 pC, u32)
Byte 20–23: pulse energy (eV, u32; set 0 for tests)
Byte 24–27: accelerator cycle (u32; set 0 for tests)
Byte 28–31: veto flags | flags (copy 0x00400003 from fixture)
Byte 32+:   one source section per distinct bank, then events
```

For single-bank test packets: one source section header (16 bytes:
source ID, intra-pulse time, COR+TOF offset, bank count), then one bank
section (8 bytes: bank ID, event count), then the events (8 bytes each:
TOF u32, pixel ID u32).

Cite `Framework/LiveData/src/ADARAPackets.cpp` for the struct layout of
`BankedEventPkt` if the byte ordering of source/bank sections is
unclear.

Define a small POD locally:
```cpp
struct PixelTof { uint32_t tof; uint32_t pixel; };
```

**`buildBeamMonitorPkt(uint64_t pulseId, uint32_t monitorId,
std::span<const uint32_t> tofs)`** — reference
`beamMonitorPacketV1[136]` (layout diagram in `ADARAPackets.h` at line
~92). *Justification:* TOF count varies per call.

**`buildRunInfoPkt(const std::string& proposalId, const std::string&
title)`** — reference `runInfoPacketV0[92]`. *Justification:* XML
payload varies per call. Build XML as
`"<proposal_id>" + proposalId + "</proposal_id><title>" + title +
"</title>"`; pad to a 4-byte boundary; update payload length (bytes
0–3) and XML length (bytes 16–19).

**`buildGeometryPkt(const std::string& xml)`** — reference
`geometryPacketV0[92]`. *Justification:* XML payload varies. Layout:
header (16 bytes) + XML length (4-byte LE u32) + XML bytes + padding.
*For tests that only need any valid geometry, use the
`geometryPacketV0` array verbatim via `PKT()` shorthand instead — do
not invoke this builder.*

**`buildBeamlineInfoPkt(const std::string& longName)`** — reference
`beamlineInfoPacketV1[32]`. *Justification:* name + name-length fields
vary. *For tests that only need any valid beamline info, use the array
verbatim via `PKT()` shorthand.*

**`buildDeviceDescriptorPkt(uint32_t devId, const std::string&
xmlDescriptor)`** — reference `devDesPacket[2600]`. *Justification:*
XML content and device ID vary per call. Layout:
```
Byte  0–3:  payload length
Byte  4–7:  type/ver = 0x00800000 (DEVICE_DESC v0)
Byte  8–11: timestamp seconds
Byte 12–15: timestamp nanos
Byte 16–19: device ID (LE u32)
Byte 20–23: descriptor length (LE u32, = xmlDescriptor.size())
Byte 24+:   XML bytes (padded to 4-byte boundary)
```

### 5.4 Assertion helper

Each builder that patches bytes must include a debug assert that the
produced payload-length field (bytes 0–3, LE) equals
`(total_size - 16)`:

```cpp
assert(pkt.size() >= 4);
uint32_t reported = pkt[0] | (pkt[1]<<8) | (pkt[2]<<16) | (pkt[3]<<24);
assert(reported + 16 == pkt.size());
```

---

## 6. CMake registration

Edit `Framework/LiveData/test/CMakeLists.txt` line 10 to add
`MockSMSServer.cpp` to `TESTHELPER_SRCS`:

```cmake
set(TESTHELPER_SRCS KafkaTesting.h KafkaTestThreadHelper.h
                    TestDataListener.cpp TestGroupDataListener.cpp
                    MockSMSServer.cpp)
```

Do **not** edit `Framework/LiveData/CMakeLists.txt` in this commit
(that file's `TEST_FILES` edit is in `subspec03`).
Do **not** add the `TIMEOUT 120` property in this commit (also
`subspec03`).

---

## 7. TODO

- [ ] Create `Framework/LiveData/test/MockSMSServer.h` with the public
      surface in §4 (platform guard, `ScriptEntry`, `MockSMSServer`
      class, `TestWatchdog` class, builder forward-declarations).
- [ ] Create `Framework/LiveData/test/MockSMSServer.cpp` with:
      - the platform guard `#ifndef _WIN32 … #endif`,
      - the includes in §4.4,
      - the `MockSMSServer::Impl` pimpl carrying
        `Poco::Net::ServerSocket`, `Poco::Net::StreamSocket`, the
        script vector, the mutex/CV, the background thread, and the
        watchdog deadline,
      - the constructor / destructor / `start()` / `script()` /
        `scriptAppend()` / `releaseExtractGate()` / diagnostic getters
        per §4.1–4.3,
      - the `TestWatchdog::Impl` pimpl + ctor/dtor/disarm per §4.5,
      - all builder helpers in §5.3 (verbatim copies, byte patches per
        §5.3, and variable-length assemblers), each with the assertion
        from §5.4 where applicable.
- [ ] Edit `Framework/LiveData/test/CMakeLists.txt` line 10: append
      `MockSMSServer.cpp` to `TESTHELPER_SRCS` per §6.
- [ ] Confirm that **no `Framework/LiveData/src/**` or
      `Framework/LiveData/inc/**` file is in the diff for this commit.
- [ ] Confirm that `MockSMSServer.h` includes **no** `Poco::Net::*`
      headers — the pimpl confines them to `.cpp`.
- [ ] Confirm every byte array referenced by name in §5 actually exists
      in `Framework/LiveData/test/ADARAPackets.h`. If any is missing,
      **stop** per §0 item 6.
- [ ] Confirm no builder duplicates byte sequences from `ADARAPackets.h`
      inline; every builder either reuses the array directly or patches
      a copy of it.

---

## 8. Definition of done for this commit

1. `MockSMSServer.h` and `MockSMSServer.cpp` exist under
   `Framework/LiveData/test/` with the structure in §4 and §5.
2. `MockSMSServer.cpp` is listed in `TESTHELPER_SRCS` in
   `Framework/LiveData/test/CMakeLists.txt`.
3. No other file is touched in this commit.
4. The new code is wrapped in `#ifndef _WIN32 … #endif`.
5. No `Poco::Net::*` header appears in `MockSMSServer.h`.
6. Every builder `#include`s and references `ADARAPackets.h` rather
   than inlining byte arrays.
