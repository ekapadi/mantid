# Sub-spec 03 — ADARA Packet Fixtures

**Part of:** [`00-index.md`](00-index.md) (master)  
**Agent read-order:** Read after [`02-mock-sms-server.md`](02-mock-sms-server.md).

---

## 4.3 Packet builders — canonical fixture reuse

### 4.3.1 Guiding principle

`Framework/LiveData/test/ADARAPackets.h` (SHA `42ebd4dcac52e69546e31037e9d8ee9be33c7673`
on this branch) contains a curated set of pre-built, validated packet byte arrays
and their full byte-layout diagrams in header comments.  These arrays are consumed by
`Framework/LiveData/test/ADARAPacketTest.h` and have been round-tripped through the
real `ADARA::Parser`.  They are the canonical fixtures for this codebase.

**Default rule:** `MockSMSServer.cpp` must `#include "ADARAPackets.h"` and
emit those arrays verbatim wherever a fixture exists for the needed packet
type.  Do not re-derive byte layouts.  Do not maintain parallel copies of
payloads that already live in `ADARAPackets.h`.

**Rationale:** Hand-rolling parallel packet bytes would create a second
source of truth that drifts from `ADARAPackets.h` / `ADARAPackets.cpp`.
The existing fixtures are build-validated by `ADARAPacketTest` and
round-tripped through `ADARA::Parser`; we reuse them.

**Exception rule:** Hand-rolled builders are permitted *only* for packet
types whose payload must vary per-call in ways that cannot be expressed
as a small fixed-field patch to an existing fixture.  Each such builder
must cite the corresponding struct in
`Framework/LiveData/src/ADARAPackets.cpp` by line range, with a
one-sentence justification.

### 4.3.2 Fixture inventory

Every packet type needed by the integration scenarios in
[`04-test-scenarios.md`](04-test-scenarios.md) has a pre-built fixture in
`ADARAPackets.h`.  There are **no gaps** requiring hand-rolled builders
from scratch:

| Packet type | `ADARAPackets.h` array(s) | Notes |
|---|---|---|
| Banked Event v0 | `bankedEventPacketV0[96]` | Fixed events; good for "any packet received" tests |
| Banked Event v1 | `bankedEventPacketV1[216]` | Has full byte-layout diagram with offset comments |
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
| Device Descriptor v0 | `devDesPacket[2600]` | Large XML; good for exact-fixture reuse tests |

### 4.3.3 Builder API

Non-member helpers in the `Testing` namespace return `std::vector<uint8_t>`.
For types where the fixture is usable verbatim (or with small field patches),
the builder copies the fixture array and patches the specified bytes
in-place.  For types where the payload size varies, the builder assembles
a fresh packet from header + variable section.

Builders must `#include "ADARAPackets.h"` — they **must not** duplicate the
byte arrays inline.

#### Verbatim-with-patch builders

These construct a `std::vector<uint8_t>` directly from an existing
fixture array, then patch only the fields the caller specifies.
See byte offsets from `ADARAPackets.h` comments.

---

**`buildHeartbeatPkt(uint64_t pulseId)`**

Base: `heartbeatPacketV0[16]`.

The heartbeat has no payload — only the 16-byte ADARA header.
Header layout (all fields little-endian):
```
Byte  0–3:  payload length    (0x00000000 for heartbeat)
Byte  4–7:  packet type/ver   (0x00400900 for heartbeat v0)
Byte  8–11: pulse ID seconds  (upper 32 bits of pulseId >> 32)
Byte 12–15: pulse ID nanos    (lower 32 bits of pulseId & 0xFFFFFFFF)
```

Patch bytes 8–11 with `static_cast<uint32_t>(pulseId >> 32)` (LE) and
bytes 12–15 with `static_cast<uint32_t>(pulseId & 0xFFFFFFFF)` (LE).

---

**`buildRunStatusPkt(ADARA::RunStatus::Enum status, uint32_t runNumber, uint64_t pulseId)`**

Select the base fixture by the `status` value:
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
Byte 24–27: status | flags    (patch status nibble; see ADARA::RunStatus)
Byte 28–31: paused            (leave as-is from fixture)
Byte 32–35: addendum          (leave as-is from fixture)
```

Patch bytes 8–15 with the caller-supplied `pulseId`, and bytes 16–19
with `runNumber`.  Patch bytes 24–27: keep the flags nibble from the
fixture and OR in the caller-supplied status value.

**Justification for patching (not verbatim):** The run number and pulse
timestamp must be caller-controlled for the scenarios in §6.3 and §6.5.
The fixture provides the correct type/version and flags structure; only
these three fields vary.

---

**`buildVariableU32Pkt(uint32_t devId, uint32_t pvId, uint32_t value, uint64_t pulseId)`**

Base: `variableU32Packet[32]`.

Variable U32 v0 byte layout (from `ADARAPackets.h` comments):
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

---

**`buildVariableDoublePkt(uint32_t devId, uint32_t pvId, double value, uint64_t pulseId)`**

Base: `variableDoublePacket[36]`.

Variable double v0 byte layout:
```
Byte  0–3:  payload length   = 0x00000014 (20 bytes)
Byte  4–7:  type/ver          = 0x00800200
Byte  8–11: timestamp seconds (patch with pulseId >> 32)
Byte 12–15: timestamp nanos   (patch with pulseId & 0xFFFFFFFF)
Byte 16–19: device ID         (patch with devId)
Byte 20–23: variable ID       (patch with pvId)
Byte 24–27: status/severity   (leave as-is: 0x00000000 = OK/OK)
Byte 28–35: value             (patch with double, IEEE 754 LE 8 bytes;
                               use memcpy(&pkt[28], &value, 8))
```

---

**Annotation builders — use pre-selected fixtures, patch comment length**

For pause/resume scenarios, use the pre-built typed fixtures directly
(without patching) — they already contain appropriate comments and scan
indices:

```cpp
// Pause: return verbatim copy of AnnotationPacketType3
std::vector<uint8_t> buildPausePkt() {
    return std::vector<uint8_t>(
        AnnotationPacketType3, AnnotationPacketType3 + sizeof(AnnotationPacketType3));
}

// Resume: return verbatim copy of AnnotationPacketType4
std::vector<uint8_t> buildResumePkt() {
    return std::vector<uint8_t>(
        AnnotationPacketType4, AnnotationPacketType4 + sizeof(AnnotationPacketType4));
}
```

For scan-start / scan-stop, use `AnnotationPacketType1` / `AnnotationPacketType2` verbatim.

For a generic annotation, use `AnnotationPacketType0` verbatim.

If a test requires a different comment text, the builder may patch the
comment bytes at offset 24 and update the comment length at byte 20 (LE u16
in the lower 16 bits of the type/comment-len word).  But verbatim reuse is
preferred; the scenarios in §6.7 do not require custom comment text.

#### Variable-length builders

These must assemble a fresh packet because the payload size varies.
They must use `ADARAPackets.h` as a **reference for the header structure**
(cite the matching fixture and explain why verbatim reuse is impossible),
not copy the body bytes.

---

**`buildBankedEventPkt(uint64_t pulseId, double pulseChargePc, std::span<const PixelTof> events)`**

Reference: `bankedEventPacketV1[216]` (layout diagram in `ADARAPackets.h` at line ~31).

Justification: the events section (bank ID + event array) has variable
length depending on `events.size()`.  The fixture cannot be patched in-place.

Header + fixed-section layout to replicate (from the `bankedEventPacketV1` comment):
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

For single-bank test packets: one source section header (16 bytes: source ID,
intra-pulse time, COR+TOF offset, bank count), then one bank section (8 bytes:
bank ID, event count), then the events (8 bytes each: TOF u32, pixel ID u32).

Cite `Framework/LiveData/src/ADARAPackets.cpp` for the struct layout of
`BankedEventPkt` if the exact byte ordering of the source/bank sections is
unclear.

---

**`buildBeamMonitorPkt(uint64_t pulseId, uint32_t monitorId, std::span<const uint32_t> tofs)`**

Reference: `beamMonitorPacketV1[136]` (layout diagram in `ADARAPackets.h` at line ~92).

Justification: TOF count varies per call.

---

**`buildRunInfoPkt(const std::string& proposalId, const std::string& title)`**

Reference: `runInfoPacketV0[92]`.

Justification: XML payload varies per call.

Build the XML as: `"<proposal_id>" + proposalId + "</proposal_id><title>" + title + "</title>"`.
Pad to a 4-byte boundary.  Update the payload length (bytes 0–3) and XML
length (bytes 16–19) accordingly.

---

**`buildGeometryPkt(const std::string& xml)`**

Reference: `geometryPacketV0[92]`.

Justification: XML payload varies per call.

Layout: header (16 bytes) + XML length (4 bytes, LE u32) + XML bytes + padding.
Use `geometryPacketV0` for the header structure only; patch XML length and
XML content.

For tests that only need *some* valid geometry, use `geometryPacketV0` verbatim
(it contains a minimal VACUO instrument definition) rather than building a
fresh packet.

---

**`buildBeamlineInfoPkt(const std::string& longName)`**

Reference: `beamlineInfoPacketV1[32]`.

Justification: the name field (and name-length field at byte 18-19) vary per
call.  However, for tests that only need *any* valid beamline info packet, use
`beamlineInfoPacketV1` verbatim.

---

**`buildDeviceDescriptorPkt(uint32_t devId, const std::string& xmlDescriptor)`**

Reference: `devDesPacket[2600]`.

Justification: XML content and device ID vary per call.

Device Descriptor v0 layout:
```
Byte  0–3:  payload length
Byte  4–7:  type/ver = 0x00800000 (DEVICE_DESC v0)
Byte  8–11: timestamp seconds
Byte 12–15: timestamp nanos
Byte 16–19: device ID (LE u32)
Byte 20–23: descriptor length (LE u32, = xmlDescriptor.size())
Byte 24+:   XML bytes (padded to 4-byte boundary)
```

For tests that only need device-descriptor→variable-value pairing and do not
inspect the XML, use `devDesPacket` verbatim (device ID 1 = `lakesHore_336`).

### 4.3.4 Builder invocation example

```cpp
// In a test: send a minimal run lifecycle
m_server->script({
    std::vector<uint8_t>(geometryPacketV0,
                         geometryPacketV0 + sizeof(geometryPacketV0)),
    std::vector<uint8_t>(beamlineInfoPacketV1,
                         beamlineInfoPacketV1 + sizeof(beamlineInfoPacketV1)),
    Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, /*runNum=*/42,
                                /*pulseId=*/0x0000000100000000ULL),
    std::vector<uint8_t>(bankedEventPacketV1,
                         bankedEventPacketV1 + sizeof(bankedEventPacketV1)),
    Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, /*runNum=*/42,
                                /*pulseId=*/0x0000000200000000ULL),
    PktDisconnect{},
});
```

For verbatim fixture packets that need no patching, construct the
`std::vector<uint8_t>` directly from the array:
```cpp
std::vector<uint8_t>(arrayName, arrayName + sizeof(arrayName))
```

For brevity in the scenario descriptions in
[`04-test-scenarios.md`](04-test-scenarios.md), we use shorthand
`PKT(name)` to mean `std::vector<uint8_t>(name, name + sizeof(name))`.

### 4.3.5 Assertion helper

Each builder that patches bytes must include a debug assert that the
produced payload-length field (bytes 0–3, LE) equals
`(total_size - 16)` (i.e. total size minus the 16-byte ADARA header):

```cpp
assert(pkt.size() >= 4);
uint32_t reported = pkt[0] | (pkt[1]<<8) | (pkt[2]<<16) | (pkt[3]<<24);
assert(reported + 16 == pkt.size());
```
