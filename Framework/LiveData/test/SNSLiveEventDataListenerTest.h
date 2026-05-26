// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright &copy; 2018 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX - License - Identifier: GPL - 3.0 +
#pragma once
#ifndef _WIN32

// INTEGRATION TEST.  Drives a real SNSLiveEventDataListener against an
// in-process MockSMSServer over a Unix-domain socket.  Does NOT require
// SMS or any external network resource.  Linux/macOS only — compiles
// to an empty suite on Windows.
//
// For the deferred-run-details invariant see
// SNSLiveEventDataListenerNoNetworkTest.h::test_*.

#include <cxxtest/TestSuite.h>

#include "MantidAPI/ILiveListener.h"
#include "MantidAPI/Workspace_fwd.h"
#include "MantidKernel/ConfigService.h"
#include "MantidLiveData/SNSLiveEventDataListener.h"
#include "MantidTypes/Core/DateAndTime.h"

#include "ADARAPackets.h"  // every byte-array fixture
#include "MockSMSServer.h" // from subspec02

#include <Poco/Net/SocketAddress.h>
#include <Poco/TemporaryFile.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace Mantid;
using namespace Mantid::LiveData;

// Construct a std::vector<uint8_t> from a fixture array in ADARAPackets.h.
#define PKT(name) std::vector<uint8_t>((name), (name) + sizeof(name))

namespace {

/// Spin-wait up to @p timeout, polling every @p poll, until @p pred
/// returns true.  On timeout calls TS_FAIL and returns false.
template <typename Pred>
bool waitFor(Pred pred,
             std::chrono::milliseconds timeout = std::chrono::seconds{5},
             std::chrono::milliseconds poll = std::chrono::milliseconds{10}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!pred()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      TS_FAIL("waitFor timed out");
      return false;
    }
    std::this_thread::sleep_for(poll);
  }
  return true;
}

/// Wraps listener.extractData() in std::async.  On timeout (default
/// 10 s) calls TS_FAIL and returns nullptr.  Protects against mutex
/// deadlocks in onBeforeExtract / onBeginRun / onEndRun.
inline std::shared_ptr<API::Workspace>
extractWithTimeout(SNSLiveEventDataListener &listener,
                   std::chrono::seconds timeout = std::chrono::seconds{10}) {
  auto fut = std::async(std::launch::async, [&] { return listener.extractData(); });
  if (fut.wait_for(timeout) == std::future_status::timeout) {
    TS_FAIL("extractData() timed out — possible deadlock");
    return nullptr;
  }
  return fut.get();
}

} // namespace

/// Notes:
/// - No listener is constructed in the fixture constructor.  Each test
///   calls connectListener() *after* queuing the script.  Some tests
///   (e.g. test_connectFailure_returnsFalse in subspec06) want to drive
///   connect() against a server that is *not* yet listening, so the
///   fixture must not eagerly connect.
/// - connectListener() calls connect() first, then start().
///   This is the required lifecycle: the socket must be connected before
///   the background thread can begin reading.
/// - The UDS address is built via Poco::Net::AddressFamily::UNIX_LOCAL
///   using the two-argument SocketAddress(AddressFamily::UNIX_LOCAL, path)
///   constructor.  Do NOT set the SNSLiveEventDataListener.testAddress
///   config key; that key supports only TCP "host:port" form.
/// - Destruction order in tearDown is strict: listener -> server ->
///   sockfile -> watchdog.  Reordering risks the listener's bg thread
///   reading from a destroyed socket.
class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {
public:
  static SNSLiveEventDataListenerTest *createSuite() { return new SNSLiveEventDataListenerTest(); }
  static void destroySuite(SNSLiveEventDataListenerTest *s) { delete s; }

  void setUp() override {
    m_sockFileHandle = std::make_unique<Poco::TemporaryFile>();
    m_sockPath = m_sockFileHandle->path();
    if (m_sockPath.size() >= 100) {
      TS_SKIP("UDS path too long for sun_path: " + m_sockPath);
      return;
    }
    // bind() requires the path to NOT exist:
    std::filesystem::remove(m_sockPath);

    // Save current config; restore in tearDown.
    auto &cfg = Kernel::ConfigService::Instance();
    m_savedKeepPausedEvents = cfg.getString("SNSLiveEventDataListener.keepPausedEvents");

    m_server = std::make_unique<Testing::MockSMSServer>(m_sockPath);
    m_watchdog = std::make_unique<Testing::TestWatchdog>(std::chrono::seconds{60}, "SNSLiveEventDataListenerTest");
  }

  void tearDown() override {
    // Strict destruction order: listener -> server -> sockfile.
    // The listener's bg thread holds a socket that may be reading;
    // join it before destroying the server.
    m_listener.reset();
    m_server.reset();
    m_sockFileHandle.reset();
    m_watchdog.reset(); // disarm last

    auto &cfg = Kernel::ConfigService::Instance();
    cfg.setString("SNSLiveEventDataListener.keepPausedEvents", m_savedKeepPausedEvents);
  }

  // ----- placeholder test (only test in this commit) -----
  void test_LegacyConstruction_initialState() {
    // Construct the listener WITHOUT calling connectListener();
    // this test never opens the socket.  See subspec04 §6.1 for
    // the rationale — this test must observe the disconnected
    // initial state.
    m_listener = std::make_unique<SNSLiveEventDataListener>();
    TS_ASSERT(!m_listener->isConnected());
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::NoRun);
  }

  // ----- (additional test_* methods added in subspec04 / 05 / 06) -----

private:
  // Each behavioural test calls this AFTER queuing the server script.
  // Returns true on success.  Builds the UDS SocketAddress via the
  // AddressFamily::UNIX_LOCAL enum form — NOT a "host:port" string.
  bool connectListener(Types::Core::DateAndTime startTime = Types::Core::DateAndTime()) {
    m_listener = std::make_unique<SNSLiveEventDataListener>();
    Poco::Net::SocketAddress udsAddr(Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath);
    if (!m_listener->connect(udsAddr))
      return false;
    m_listener->start(startTime);
    return true;
  }

  std::string m_savedKeepPausedEvents;
  std::unique_ptr<Poco::TemporaryFile> m_sockFileHandle;
  std::string m_sockPath;
  std::unique_ptr<Testing::MockSMSServer> m_server;
  std::unique_ptr<SNSLiveEventDataListener> m_listener;
  std::unique_ptr<Testing::TestWatchdog> m_watchdog;
};

#else
// Windows stub
class SNSLiveEventDataListenerTest : public CxxTest::TestSuite {};
#endif // !_WIN32
