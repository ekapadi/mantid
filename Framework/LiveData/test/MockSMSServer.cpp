// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright © 2018 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX-License-Identifier: GPL-3.0+

#ifndef _WIN32

#include "MockSMSServer.h"

#include "MantidKernel/Logger.h"

#include <Poco/Net/SocketAddress.h>

#include <cassert>
#include <cstring>
#include <stdexcept>
#include <unistd.h> // ::unlink

namespace {
Mantid::Kernel::Logger g_log("MockSMSServer");

// ---------------------------------------------------------------------------
// Little-endian serialisation helpers
// ---------------------------------------------------------------------------
void le32(std::vector<uint8_t> &buf, uint32_t v) {
  buf.push_back(static_cast<uint8_t>(v & 0xFFu));
  buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
  buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
  buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

/// Append a 16-byte ADARA packet header.
/// @param payloadLen  Bytes in the payload (must be a multiple of 4).
/// @param baseType    ADARA PacketType enum value (e.g. 0x4003 for RunStatus).
/// @param version     Protocol version byte.
/// @param pulseId     (ts_sec << 32) | ts_nsec  — EPICS epoch seconds in upper 32 bits.
void appendHeader(std::vector<uint8_t> &buf, uint32_t payloadLen, uint32_t baseType, uint8_t version,
                  uint64_t pulseId) {
  le32(buf, payloadLen);
  le32(buf, ADARA_PKT_TYPE(baseType, version));
  le32(buf, static_cast<uint32_t>(pulseId >> 32));  // ts_sec  (EPICS epoch seconds)
  le32(buf, static_cast<uint32_t>(pulseId & 0xFFFFFFFFu)); // ts_nsec
}

/// Append padding zeros so that buf.size() is a multiple of 4.
void pad4(std::vector<uint8_t> &buf, std::size_t contentBytes) {
  const std::size_t rem = contentBytes % 4;
  if (rem != 0) {
    for (std::size_t i = 0; i < (4 - rem); ++i)
      buf.push_back(0u);
  }
}
} // anonymous namespace

namespace Mantid::LiveData::Testing {

// ---------------------------------------------------------------------------
// Minimal instrument XML
// ---------------------------------------------------------------------------

const std::string &minimalInstrumentXml() {
  // Instrument "TESTINST": source, sample, 1 detector (ID 1), 1 monitor (ID -1).
  // No <parameter>/<logfile> nodes → m_requiredLogs stays empty.
  static const std::string kXml =
      "<?xml version='1.0' encoding='ASCII'?>"
      "<instrument name=\"TESTINST\" "
      "valid-from=\"1900-01-31 23:59:59\" "
      "valid-to=\"2100-01-31 23:59:59\" "
      "last-modified=\"2021-01-01 00:00:00\">"
      "<defaults>"
      "<length unit=\"metre\"/>"
      "<angle unit=\"degree\"/>"
      "<reference-frame>"
      "<along-beam axis=\"z\"/>"
      "<pointing-up axis=\"y\"/>"
      "<handedness val=\"right\"/>"
      "</reference-frame>"
      "</defaults>"
      "<component type=\"sourcetype\"><location z=\"-10.0\"/></component>"
      "<type name=\"sourcetype\" is=\"Source\"/>"
      "<component type=\"sampletype\"><location/></component>"
      "<type name=\"sampletype\" is=\"SamplePos\"/>"
      "<component type=\"monitors\" idlist=\"mon_ids\"><location/></component>"
      "<type name=\"monitors\">"
      "<component type=\"monitor\"><location z=\"-2.5\" name=\"mon1\"/></component>"
      "</type>"
      "<type name=\"monitor\" is=\"monitor\">"
      "<sphere id=\"shape\">"
      "<centre x=\"0.0\" y=\"0.0\" z=\"0.0\"/>"
      "<radius val=\"0.01\"/>"
      "</sphere>"
      "<algebra val=\"shape\"/>"
      "</type>"
      "<idlist idname=\"mon_ids\"><id val=\"-1\"/></idlist>"
      "<component type=\"detectors\" idlist=\"det_ids\"><location/></component>"
      "<type name=\"detectors\">"
      "<component type=\"pixel\">"
      "<location r=\"1\" t=\"90\" p=\"0\" name=\"det1\"/>"
      "</component>"
      "</type>"
      "<type name=\"pixel\" is=\"detector\">"
      "<cuboid id=\"shape\">"
      "<left-front-bottom-point x=\"-0.005\" y=\"-0.005\" z=\"0.0\"/>"
      "<left-front-top-point x=\"-0.005\" y=\"0.005\" z=\"0.0\"/>"
      "<left-back-bottom-point x=\"-0.005\" y=\"-0.005\" z=\"-0.001\"/>"
      "<right-front-bottom-point x=\"0.005\" y=\"-0.005\" z=\"0.0\"/>"
      "</cuboid>"
      "<algebra val=\"shape\"/>"
      "</type>"
      "<idlist idname=\"det_ids\"><id val=\"1\"/></idlist>"
      "</instrument>";
  return kXml;
}

// ---------------------------------------------------------------------------
// Packet builders
// ---------------------------------------------------------------------------

std::vector<uint8_t> buildGeometryPkt(const std::string &xml, uint64_t pulseId) {
  const uint32_t xmlLen = static_cast<uint32_t>(xml.size());
  const uint32_t payloadLen = 4 + ((xmlLen + 3) & ~3u);
  std::vector<uint8_t> buf;
  buf.reserve(16 + payloadLen);
  appendHeader(buf, payloadLen, ADARA::PacketType::GEOMETRY_TYPE, 0, pulseId);
  le32(buf, xmlLen);
  buf.insert(buf.end(), xml.begin(), xml.end());
  pad4(buf, xmlLen);
  return buf;
}

std::vector<uint8_t> buildBeamlineInfoPkt(const std::string &longName, uint64_t pulseId) {
  // Sizes field: bits[31:24]=targetStation, bits[23:16]=idLen, bits[15:8]=shortLen, bits[7:0]=longLen
  const std::string id = "1";
  const std::string shortName = "TST";
  const std::string allStrs = id + shortName + longName;
  const uint32_t totalLen = static_cast<uint32_t>(allStrs.size());
  const uint32_t payloadLen = 4 + ((totalLen + 3) & ~3u);

  uint32_t sizes = 0;
  sizes |= (static_cast<uint32_t>(id.size()) << 16);
  sizes |= (static_cast<uint32_t>(shortName.size()) << 8);
  sizes |= static_cast<uint32_t>(longName.size() & 0xFFu);

  std::vector<uint8_t> buf;
  buf.reserve(16 + payloadLen);
  appendHeader(buf, payloadLen, ADARA::PacketType::BEAMLINE_INFO_TYPE, 0, pulseId);
  le32(buf, sizes);
  buf.insert(buf.end(), allStrs.begin(), allStrs.end());
  pad4(buf, totalLen);
  return buf;
}

std::vector<uint8_t> buildRunStatusPkt(ADARA::RunStatus::Enum status, uint32_t runNumber, uint64_t pulseId) {
  // V1 payload: 5 × uint32 = 20 bytes
  // fields[0]=runNumber, fields[1]=runStart(EPICS sec), fields[2]=(status<<24)|flags
  const uint32_t payloadLen = 20;
  const uint32_t runStart = static_cast<uint32_t>(pulseId >> 32); // EPICS seconds

  std::vector<uint8_t> buf;
  buf.reserve(36);
  appendHeader(buf, payloadLen, ADARA::PacketType::RUN_STATUS_TYPE, 1, pulseId);
  le32(buf, runNumber);
  le32(buf, runStart);
  le32(buf, static_cast<uint32_t>(status) << 24);
  le32(buf, 0);
  le32(buf, 0);
  return buf;
}

std::vector<uint8_t> buildRunInfoPkt(const std::string &proposalId, const std::string &title, uint64_t pulseId) {
  const std::string xml = "<?xml version=\"1.0\"?>"
                          "<runinfo><proposal_id>" +
                          proposalId + "</proposal_id><run_title>" + title + "</run_title></runinfo>";
  const uint32_t xmlLen = static_cast<uint32_t>(xml.size());
  const uint32_t payloadLen = 4 + ((xmlLen + 3) & ~3u);

  std::vector<uint8_t> buf;
  buf.reserve(16 + payloadLen);
  appendHeader(buf, payloadLen, ADARA::PacketType::RUN_INFO_TYPE, 0, pulseId);
  le32(buf, xmlLen);
  buf.insert(buf.end(), xml.begin(), xml.end());
  pad4(buf, xmlLen);
  return buf;
}

std::vector<uint8_t> buildBankedEventPkt(uint64_t pulseId, double pulseChargePc,
                                          const std::vector<PixelTof> &events) {
  // V1 structure:
  //   Pulse info: 4 fields (pulse charge, energy, cycle, flags)
  //   Source header: 4 fields (srcId, intraPulseTime, COR/tofOffset, bankCount)
  //   Bank header:   2 fields (bankId, eventCount)
  //   Events:        2 fields each (tof, pixel)
  const uint32_t nEvents = static_cast<uint32_t>(events.size());
  const uint32_t payloadLen = (4 + 4 + 2 + 2 * nEvents) * 4;
  const uint32_t charge = static_cast<uint32_t>(pulseChargePc / 10.0);

  std::vector<uint8_t> buf;
  buf.reserve(16 + payloadLen);
  appendHeader(buf, payloadLen, ADARA::PacketType::BANKED_EVENT_TYPE, 1, pulseId);
  // Pulse info
  le32(buf, charge); // pulseCharge (units of 10 pC)
  le32(buf, 0);      // pulseEnergy
  le32(buf, 0);      // accel cycle
  le32(buf, 0);      // veto/flags
  // Source section header
  le32(buf, 0);            // sourceID
  le32(buf, 0);            // intraPulseTime
  le32(buf, 0x80000000u);  // COR flag set (tof already corrected), offset = 0
  le32(buf, 1);            // bankCount = 1
  // Bank 0 header
  le32(buf, 0);       // bankID
  le32(buf, nEvents); // eventCount
  // Events: tof(4), pixel(4)
  for (const auto &e : events) {
    le32(buf, e.tof);
    le32(buf, e.pixel);
  }
  return buf;
}

std::vector<uint8_t> buildBeamMonitorPkt(uint64_t pulseId, uint32_t monitorId,
                                          const std::vector<uint32_t> &tofs) {
  // V1 structure:
  //   Pulse info: 4 fields
  //   Section header: 3 fields (monitorId<<22 | eventCount, srcId, tofOffset+COR)
  //   Events: 1 field each
  const uint32_t nTofs = static_cast<uint32_t>(tofs.size());
  const uint32_t payloadLen = (4 + 3 + nTofs) * 4;
  const uint32_t TOF_MASK = 0x001FFFFFu;

  std::vector<uint8_t> buf;
  buf.reserve(16 + payloadLen);
  appendHeader(buf, payloadLen, ADARA::PacketType::BEAM_MONITOR_EVENT_TYPE, 1, pulseId);
  // Pulse info
  le32(buf, 0); le32(buf, 0); le32(buf, 0); le32(buf, 0);
  // Section header
  le32(buf, ((monitorId & 0x3FFu) << 22) | (nTofs & 0x3FFFFFu));
  le32(buf, 0);            // sourceID
  le32(buf, 0x80000000u);  // corrected flag set, offset = 0
  // Events: rising edge(bit31) | cycle(bits30-21) | tof(bits20-0)
  for (const uint32_t tof : tofs)
    le32(buf, 0x80000000u | (tof & TOF_MASK));
  return buf;
}

std::vector<uint8_t> buildAnnotationPkt(ADARA::MarkerType::Enum markerType, uint32_t scanIndex,
                                         const std::string &comment, uint64_t pulseId) {
  const uint16_t commentLen = static_cast<uint16_t>(comment.size());
  const uint32_t payloadLen = 8 + ((commentLen + 3) & ~3u);

  std::vector<uint8_t> buf;
  buf.reserve(16 + payloadLen);
  appendHeader(buf, payloadLen, ADARA::PacketType::STREAM_ANNOTATION_TYPE, 0, pulseId);
  // fields[0]: (resetHint=0 << 31) | (markerType << 16) | commentLen
  le32(buf, (static_cast<uint32_t>(markerType) << 16) | static_cast<uint32_t>(commentLen));
  le32(buf, scanIndex);
  if (commentLen > 0) {
    buf.insert(buf.end(), comment.begin(), comment.end());
    pad4(buf, commentLen);
  }
  return buf;
}

std::vector<uint8_t> buildDeviceDescriptorPkt(uint32_t devId, const std::vector<PVDesc> &pvs, uint64_t pulseId) {
  std::string xml = "<device><process_variables>";
  for (const auto &pv : pvs) {
    xml += "<process_variable>";
    xml += "<pv_name>" + pv.pvName + "</pv_name>";
    xml += "<pv_id>" + std::to_string(pv.pvId) + "</pv_id>";
    xml += "<pv_type>" + pv.pvType + "</pv_type>";
    xml += "</process_variable>";
  }
  xml += "</process_variables></device>";

  const uint32_t xmlLen = static_cast<uint32_t>(xml.size());
  const uint32_t payloadLen = 8 + ((xmlLen + 3) & ~3u);

  std::vector<uint8_t> buf;
  buf.reserve(16 + payloadLen);
  appendHeader(buf, payloadLen, ADARA::PacketType::DEVICE_DESC_TYPE, 0, pulseId);
  le32(buf, devId);
  le32(buf, xmlLen);
  buf.insert(buf.end(), xml.begin(), xml.end());
  pad4(buf, xmlLen);
  return buf;
}

std::vector<uint8_t> buildVariableU32Pkt(uint32_t devId, uint32_t pvId, uint32_t value, uint64_t pulseId) {
  // V0 payload: exactly 4 × uint32 = 16 bytes
  std::vector<uint8_t> buf;
  buf.reserve(32);
  appendHeader(buf, 16, ADARA::PacketType::VAR_VALUE_U32_TYPE, 0, pulseId);
  le32(buf, devId);
  le32(buf, pvId);
  le32(buf, 0); // status=OK (bits 31:16), severity=OK (bits 15:0)
  le32(buf, value);
  return buf;
}

std::vector<uint8_t> buildVariableDoublePkt(uint32_t devId, uint32_t pvId, double value, uint64_t pulseId) {
  // V0 payload: 3 × uint32 + 1 × double = 12 + 8 = 20 bytes
  std::vector<uint8_t> buf;
  buf.reserve(36);
  appendHeader(buf, 20, ADARA::PacketType::VAR_VALUE_DOUBLE_TYPE, 0, pulseId);
  le32(buf, devId);
  le32(buf, pvId);
  le32(buf, 0); // status=OK, severity=OK
  uint8_t dblBytes[8];
  std::memcpy(dblBytes, &value, 8);
  buf.insert(buf.end(), dblBytes, dblBytes + 8);
  return buf;
}

std::vector<uint8_t> buildHeartbeatPkt(uint64_t pulseId) {
  // V0: zero payload
  std::vector<uint8_t> buf;
  buf.reserve(16);
  appendHeader(buf, 0, ADARA::PacketType::HEARTBEAT_TYPE, 0, pulseId);
  return buf;
}

// ---------------------------------------------------------------------------
// TestWatchdog
// ---------------------------------------------------------------------------

TestWatchdog::TestWatchdog(std::chrono::seconds timeout, std::string testName)
    : m_testName(std::move(testName)) {
  m_thread = std::thread([this, timeout]() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cv.wait_for(lock, timeout, [this] { return m_disarmed; });
    if (!m_disarmed) {
      g_log.fatal() << "TestWatchdog: test '" << m_testName << "' exceeded its time limit — aborting.\n";
      std::abort();
    }
  });
}

TestWatchdog::~TestWatchdog() {
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_disarmed = true;
  }
  m_cv.notify_all();
  if (m_thread.joinable())
    m_thread.join();
}

// ---------------------------------------------------------------------------
// MockSMSServer
// ---------------------------------------------------------------------------

MockSMSServer::MockSMSServer(std::string path) : m_path(std::move(path)) {}

MockSMSServer::~MockSMSServer() {
  m_stop = true;
  // Unblock the gate condvar so the thread can exit PktWaitForExtract.
  releaseExtractGate();
  try {
    m_clientSocket.shutdown();
  } catch (...) {
  }
  try {
    m_listenSocket.close();
  } catch (...) {
  }
  if (m_thread.joinable())
    m_thread.join();
  ::unlink(m_path.c_str());
}

void MockSMSServer::start() {
  Poco::Net::SocketAddress addr(Poco::Net::SocketAddress::UNIX_LOCAL, m_path);
  m_listenSocket.bind(addr);
  m_listenSocket.listen(1);
  m_thread = std::thread(&MockSMSServer::run, this);
}

void MockSMSServer::script(std::initializer_list<ScriptEntry> entries) {
  std::lock_guard<std::mutex> lock(m_scriptMutex);
  m_script.assign(entries.begin(), entries.end());
}

void MockSMSServer::scriptAppend(ScriptEntry entry) {
  std::lock_guard<std::mutex> lock(m_scriptMutex);
  m_script.emplace_back(std::move(entry));
}

void MockSMSServer::releaseExtractGate() {
  {
    std::lock_guard<std::mutex> lock(m_gateMutex);
    m_gateOpen = true;
  }
  m_gateCV.notify_all();
}

bool MockSMSServer::clientConnected() const { return m_clientConnected.load(); }

std::size_t MockSMSServer::bytesSent() const { return m_bytesSent.load(); }

std::size_t MockSMSServer::scriptIndex() const { return m_scriptIndex.load(); }

void MockSMSServer::run() {
  // Accept the first (and only) client.
  while (!m_stop) {
    if (m_listenSocket.poll(Poco::Timespan(0, 50'000 /*µs*/), Poco::Net::Socket::SELECT_READ)) {
      try {
        m_clientSocket = m_listenSocket.acceptConnection();
        m_clientConnected = true;
      } catch (...) {
      }
      break;
    }
  }
  if (m_stop || !m_clientConnected)
    return;

  // Drain the script.
  while (!m_stop) {
    ScriptEntry entry;
    {
      std::lock_guard<std::mutex> lock(m_scriptMutex);
      if (m_scriptIndex >= m_script.size())
        break;
      entry = m_script[m_scriptIndex];
    }

    bool done = std::visit(
        [&](auto &&arg) -> bool {
          using T = std::decay_t<decltype(arg)>;
          if constexpr (std::is_same_v<T, PktRaw>) {
            if (!arg.empty()) {
              try {
                const int sent = m_clientSocket.sendBytes(arg.data(), static_cast<int>(arg.size()));
                if (sent > 0)
                  m_bytesSent += static_cast<std::size_t>(sent);
              } catch (...) {
                m_stop = true;
                return true;
              }
            }
          } else if constexpr (std::is_same_v<T, PktWaitForExtract>) {
            std::unique_lock<std::mutex> lock(m_gateMutex);
            m_gateCV.wait(lock, [&] { return m_gateOpen || m_stop.load(); });
            m_gateOpen = false; // reset for potential reuse
          } else if constexpr (std::is_same_v<T, PktDisconnect>) {
            try {
              m_clientSocket.close();
            } catch (...) {
            }
            m_stop = true;
            return true;
          }
          return false;
        },
        entry);

    m_scriptIndex++;
    if (done)
      break;
  }
}

} // namespace Mantid::LiveData::Testing

#endif // !_WIN32
