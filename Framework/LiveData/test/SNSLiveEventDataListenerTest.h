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
#include "MantidAPI/MatrixWorkspace.h"
#include "MantidAPI/Run.h"
#include "MantidAPI/Workspace_fwd.h"
#include "MantidGeometry/Instrument.h"
#include "MantidDataObjects/EventWorkspace.h"
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

/// Instrument name advertised in the BeamlineInfo packet.  Must match the
/// `name` attribute of @ref kMinimalIDF so that LoadInstrument's IDS cache
/// keys consistently across tests.
constexpr const char *kInstrumentName = "xmlInst";

/// Minimal but VALID Mantid IDF used by the integration tests.  The ADARA
/// `geometryPacketV0` fixture in ADARAPackets.h carries the placeholder XML
/// `<instrument>VACUO</instrument>`, which is well-formed XML but is NOT a
/// valid Mantid instrument definition; feeding it to LoadInstrument inside
/// initWorkspacePart2() raises a SAXParseException, the listener's
/// background thread exits, and every behavioural test downstream of the
/// geometry+beamline handshake stalls in waitFor() or deadlocks on
/// extractData().  Use this IDF (with idlist 1..10, covering pixel=1 from
/// buildBankedEventPkt) via Testing::buildGeometryPkt() instead.
inline const std::string kMinimalIDF =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
    "<instrument name=\"xmlInst\" valid-from=\"1900-01-31 23:59:59\" "
    "valid-to=\"2100-01-31 23:59:59\" "
    "last-modified=\"2010-10-06T16:21:30\">"
    "<defaults />"
    "<component type=\"panel\" idlist=\"idlist_for_bank1\">"
    "<location r=\"0\" t=\"0\" rot=\"0\" axis-x=\"0\" axis-y=\"1\" "
    "axis-z=\"0\" name=\"bank1\" xpixels=\"3\" ypixels=\"2\" />"
    "</component>"
    "<type is=\"detector\" name=\"panel\">"
    "<properties/>"
    "<component type=\"pixel\">"
    "<location y=\"1\" x=\"1\"/>"
    "</component>"
    "</type>"
    "<type is=\"detector\" name=\"pixel\">"
    "<cuboid id=\"pixel-shape\" />"
    "<algebra val=\"pixel-shape\"/>"
    "</type>"
    "<idlist idname=\"idlist_for_bank1\">"
    "<id start=\"1\" end=\"10\" />"
    "</idlist>"
    "</instrument>";

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
      TS_FAIL("UDS path too long for sun_path (>= 100 chars): " + m_sockPath);
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

  // ----- §6.1 Legacy behavioural contract (remainder) -----

  void test_LegacyConnectAndDisconnect() {
    m_server->script({ Testing::PktDisconnect{} });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return !m_listener->isConnected(); }, std::chrono::seconds{5});
    TS_ASSERT(!m_listener->isConnected());
  }

  void test_LegacyExtractEmptyWorkspace() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::NoRun);
  }

  void test_LegacyConnectionStatusTransitions() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});
    // After receiving Geometry and BeamlineInfo, listener is Connected.
    TS_ASSERT_EQUALS(m_listener->listenerState(),
                     API::ListenerState::Connected);
    m_server->releaseExtractGate();
  }

  // ----- §6.2 Connection & mid-run join -----

  void test_connect_succeeds_over_uds() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});
    TS_ASSERT(m_server->clientConnected());
    TS_ASSERT_LESS_THAN(0u, m_server->bytesSent());
  }

  void test_midRunJoin_doesNotWipeWorkspaceInit() {
    // NEW_RUN arrives BEFORE geometry/beamline metadata — mid-run join.
    m_server->script({
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                    /*runNum=*/100,
                                    /*pulseId=*/0x0000000100000000ULL),
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        PKT(bankedEventPacketV1),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    // The listener must report Running after a NEW_RUN + events.
    TS_ASSERT_EQUALS(m_listener->runStatus(),
                     API::ILiveListener::Running);
    // Workspace must not be uninitialised (no empty instrument).
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_DIFFERS(ews->getInstrument()->getName(), std::string{});
  }

  // ----- §6.3 Single & full run lifecycle -----

  void test_singleRun_extractsEventsAndRunNumber() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 42,
                                    0x0000000100000000ULL),
        Testing::buildBankedEventPkt(0x0000000100000000ULL,
                                      /*chargePc=*/1000.0,
                                      {{/*tof=*/100u, /*pixel=*/1u}}),
        Testing::PktWaitForExtract{},
        Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 42,
                                    0x0000000200000000ULL),
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_EQUALS(
        ews->run().getPropertyValueAsType<int>("run_number"), 42);
    TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
  }

  void test_fullRun_beginExtractEndExtract() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 55,
                                    0x0000000100000000ULL),
        PKT(bankedEventPacketV1),
        Testing::PktWaitForExtract{},      // gate 1
        Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 55,
                                    0x0000000300000000ULL),
        Testing::PktWaitForExtract{},      // gate 2
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});
    // First extract
    auto ws1 = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::Running);
    m_server->releaseExtractGate(); // release gate 1
    waitFor([&]{ return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});
    // Second extract
    auto ws2 = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate(); // release gate 2
    TS_ASSERT_DIFFERS(ws1, nullptr);
    TS_ASSERT_DIFFERS(ws2, nullptr);
    TS_ASSERT_EQUALS(m_listener->runStatus(),
                     API::ILiveListener::EndRun);
  }

  void test_runNumber_proposalId_title_propagate() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 77,
                                    0x0000000100000000ULL),
        Testing::buildRunInfoPkt("IPTS-12345", "My Test Title"),
        PKT(bankedEventPacketV1),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto mws = std::dynamic_pointer_cast<API::MatrixWorkspace>(ws);
    TS_ASSERT_DIFFERS(mws, nullptr);
    const auto &run = mws->run();
    TS_ASSERT_EQUALS(
        run.getPropertyValueAsType<std::string>("experiment_identifier"),
        std::string{"IPTS-12345"});
    TS_ASSERT_EQUALS(
        run.getPropertyValueAsType<std::string>("run_title"),
        std::string{"My Test Title"});
  }

  // ----- (additional test_* methods added in subspec05 / 06) -----

  /// Regression: a malformed instrument geometry XML must surface a
  /// background exception via extractData() instead of merely letting
  /// the bg thread die and the caller spin for 10 s before getting
  /// "Exception::NotYet".  See PR comment 4550830796.
  void test_BadGeometryXml_surfacesAsExtractDataException() {
    // Deliberately invalid IDF: well-formed XML, but not a Mantid instrument
    // definition.  LoadInstrument's parser will reject this and throw a
    // SAXParseException from initWorkspacePart2().
    const std::string kBadIDF = "<not-a-valid-instrument/>";
    m_server->script({
        Testing::buildGeometryPkt(kBadIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());

    // Wait for the bg thread to have consumed both packets and attempted
    // initWorkspacePart2() (which must fail).
    waitFor([&] { return !m_listener->isConnected(); }, std::chrono::seconds{5});

    // The caller must see a real exception, NOT Exception::NotYet, and the
    // message must carry our InstrumentName context so the failure is
    // diagnosable in production.
    bool threw = false;
    std::string what;
    try {
      m_listener->extractData();
    } catch (const std::exception &e) {
      threw = true;
      what = e.what();
    }
    TSM_ASSERT("extractData() must throw when bg thread aborted in init", threw);
    TSM_ASSERT_DIFFERS("extractData() must not return Exception::NotYet here",
                       what.find("LoadInstrument failed"), std::string::npos);
    TSM_ASSERT_DIFFERS("error message must include InstrumentName context",
                       what.find(kInstrumentName), std::string::npos);
  }

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
