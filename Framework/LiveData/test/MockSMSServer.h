// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright © 2018 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX-License-Identifier: GPL-3.0+
#pragma once

// MockSMSServer — lightweight in-process ADARA server for integration tests.
// See plans/v3_integration_test/SNSListener-integration-test.md §3-§5.
//
// Usage:
//   m_server = std::make_unique<MockSMSServer>("/tmp/test_sns_<pid>.sock");
//   m_server->script({ buildGeometryPkt(...), buildRunStatusPkt(...), ... });
//   m_server->start();                     // bind + listen + thread
//   // create listener, connect, start ...
//   m_server->releaseExtractGate();        // unblock PktWaitForExtract if needed

#ifndef _WIN32

#include "MantidLiveData/ADARA/ADARA.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/StreamSocket.h>

namespace Mantid::LiveData::Testing {

// ---------------------------------------------------------------------------
// Script entry variants
// ---------------------------------------------------------------------------

/// Raw bytes to send verbatim (used for well-formed packets and garbage).
using PktRaw = std::vector<uint8_t>;

/// Disconnects the server socket — peer will receive EOF.
struct PktDisconnect {};

/// Blocks the server thread until releaseExtractGate() is called.
struct PktWaitForExtract {};

/// One entry in the MockSMSServer script.
using ScriptEntry = std::variant<PktRaw, PktDisconnect, PktWaitForExtract>;

// ---------------------------------------------------------------------------
// Packet builder helpers (declared here, defined in MockSMSServer.cpp)
// ---------------------------------------------------------------------------

struct PixelTof {
  uint32_t tof;   ///< Time-of-flight in units of 100 ns (ADARA native)
  uint32_t pixel; ///< Detector pixel ID
};

struct PVDesc {
  uint32_t pvId;
  std::string pvName;
  std::string pvType; ///< e.g. "integer", "double"
};

/// Build an ADARA Geometry (InstrumentXML) packet v0.
std::vector<uint8_t> buildGeometryPkt(const std::string &xml, uint64_t pulseId = 0);

/// Build an ADARA BeamlineInfo packet v0.
std::vector<uint8_t> buildBeamlineInfoPkt(const std::string &longName, uint64_t pulseId = 0);

/// Build an ADARA RunStatus packet v1.
std::vector<uint8_t> buildRunStatusPkt(ADARA::RunStatus::Enum status, uint32_t runNumber, uint64_t pulseId);

/// Build an ADARA RunInfo packet v0 with proposal ID and title.
std::vector<uint8_t> buildRunInfoPkt(const std::string &proposalId, const std::string &title, uint64_t pulseId = 0);

/// Build an ADARA BankedEvent packet v1 with events in a single source/bank.
std::vector<uint8_t> buildBankedEventPkt(uint64_t pulseId, double pulseChargePc,
                                          const std::vector<PixelTof> &events);

/// Build an ADARA BeamMonitor packet v1.
std::vector<uint8_t> buildBeamMonitorPkt(uint64_t pulseId, uint32_t monitorId,
                                          const std::vector<uint32_t> &tofs);

/// Build an ADARA StreamAnnotation packet v0 (PAUSE, RESUME, SCAN_START, etc.).
std::vector<uint8_t> buildAnnotationPkt(ADARA::MarkerType::Enum markerType, uint32_t scanIndex,
                                         const std::string &comment = {}, uint64_t pulseId = 0);

/// Build an ADARA DeviceDescriptor packet v0.
std::vector<uint8_t> buildDeviceDescriptorPkt(uint32_t devId, const std::vector<PVDesc> &pvs,
                                               uint64_t pulseId = 0);

/// Build an ADARA VariableValue(U32) packet v0.
std::vector<uint8_t> buildVariableU32Pkt(uint32_t devId, uint32_t pvId, uint32_t value, uint64_t pulseId = 0);

/// Build an ADARA VariableValue(Double) packet v0.
std::vector<uint8_t> buildVariableDoublePkt(uint32_t devId, uint32_t pvId, double value, uint64_t pulseId = 0);

/// Build an ADARA Heartbeat packet v0 (zero-payload).
std::vector<uint8_t> buildHeartbeatPkt(uint64_t pulseId = 0);

/// Minimal valid IDF XML for LoadInstrument: one detector (ID 1),
/// one monitor (ID -1), instrument name "TESTINST".
const std::string &minimalInstrumentXml();

// ---------------------------------------------------------------------------
// TestWatchdog — RAII abort-on-hang guard
// ---------------------------------------------------------------------------

/// Aborts the process if not destructed (disarmed) within @p timeout.
class TestWatchdog {
public:
  explicit TestWatchdog(std::chrono::seconds timeout, std::string testName = "<unknown>");
  ~TestWatchdog();

  TestWatchdog(const TestWatchdog &) = delete;
  TestWatchdog &operator=(const TestWatchdog &) = delete;

private:
  std::thread m_thread;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_disarmed{false};
  std::string m_testName;
};

// ---------------------------------------------------------------------------
// MockSMSServer
// ---------------------------------------------------------------------------

/// In-process fake SMS server that drives SNSLiveEventDataListener over a
/// Unix-domain socket.  Script entries are sent in order; PktWaitForExtract
/// blocks until releaseExtractGate() is called.
class MockSMSServer {
public:
  explicit MockSMSServer(std::string path);
  ~MockSMSServer();

  MockSMSServer(const MockSMSServer &) = delete;
  MockSMSServer &operator=(const MockSMSServer &) = delete;

  /// Bind the server socket and start the background send thread.
  void start();

  /// Replace the entire script (call before start()).
  void script(std::initializer_list<ScriptEntry> entries);

  /// Append one entry (may be called before or after start()).
  void scriptAppend(ScriptEntry entry);

  /// Unblock a PktWaitForExtract entry.
  void releaseExtractGate();

  /// True once the first client connection has been accepted.
  bool clientConnected() const;

  /// Total bytes sent to the client so far.
  std::size_t bytesSent() const;

  /// Index of the next script entry to be delivered (increases monotonically).
  std::size_t scriptIndex() const;

private:
  void run(); // background thread body

  std::string m_path;

  Poco::Net::ServerSocket m_listenSocket;
  Poco::Net::StreamSocket m_clientSocket;

  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  std::atomic<bool> m_clientConnected{false};
  std::atomic<std::size_t> m_bytesSent{0};
  std::atomic<std::size_t> m_scriptIndex{0};

  mutable std::mutex m_scriptMutex;
  std::vector<ScriptEntry> m_script;

  std::mutex m_gateMutex;
  std::condition_variable m_gateCV;
  bool m_gateOpen{false};
};

} // namespace Mantid::LiveData::Testing

#else  // _WIN32

// Minimal stubs so that the translation unit compiles on Windows even though
// no tests are exercised there.
namespace Mantid::LiveData::Testing {
struct PixelTof {
  uint32_t tof;
  uint32_t pixel;
};
struct PVDesc {
  uint32_t pvId;
  std::string pvName;
  std::string pvType;
};
struct PktDisconnect {};
struct PktWaitForExtract {};
} // namespace Mantid::LiveData::Testing

#endif // !_WIN32
