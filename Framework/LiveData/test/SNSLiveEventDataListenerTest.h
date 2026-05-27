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
/// extractData().  Use this IDF (single pixel with id=1, covering pixel=1
/// from buildBankedEventPkt) via Testing::buildGeometryPkt() instead.
inline const std::string kMinimalIDF =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
    "<instrument name=\"xmlInst\" valid-from=\"1900-01-31 23:59:59\" "
    "valid-to=\"2100-01-31 23:59:59\" "
    "last-modified=\"2010-10-06T16:21:30\">"
    "<defaults />"
    "<component type=\"panel\" idlist=\"idlist_for_bank1\">"
    "<location r=\"0\" t=\"0\" rot=\"0\" axis-x=\"0\" axis-y=\"1\" "
    "axis-z=\"0\" name=\"bank1\" />"
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
    "<id start=\"1\" end=\"1\" />"
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
    // XFAIL: The listener's background thread does not currently
    // transition isConnected() to false on a clean peer-close
    // (Poco::Net::StreamSocket::receiveBytes returns 0 with no
    // exception, which the read loop does not treat as a fatal
    // error).  The desired behaviour — once the production fix
    // lands — is that the listener observes EOF and transitions
    // out of the connected state within a few seconds of the
    // server-side close.  Following the XFAIL inversion
    // convention used in subspec06, this assertion currently
    // PASSES by observing the broken behaviour
    // (isConnected() == true after the peer closes the
    // connection); when the production fix lands the
    // inversion in the TSM_ASSERT below must be removed and
    // replaced with the intended-behaviour check
    // `waitFor([&]{ return !m_listener->isConnected(); }, ...);
    // TS_ASSERT(!m_listener->isConnected());`.
    m_server->script({ Testing::PktDisconnect{} });
    m_server->start();
    TS_ASSERT(connectListener());
    // Server-side: wait for the scripted PktDisconnect{} to complete
    // (scriptIndex advances past it once the server has closed its end).
    waitFor([&]{ return m_server->scriptIndex() >= 1; }, std::chrono::seconds{5});
    // Give the listener a brief window in which it *would* notice the
    // close if it were going to.  This is intentionally short — the
    // assertion below is XFAIL-inverted, so we are documenting that
    // even after a generous delay isConnected() remains true.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    TSM_ASSERT(
        "XFAIL: SNSLiveEventDataListener does not currently detect a "
        "clean peer-close on its data socket (Poco StreamSocket "
        "receiveBytes() returns 0 with no exception, which the bg "
        "read loop does not treat as fatal).  This assertion is "
        "INVERTED: it currently passes by observing the broken "
        "behaviour (isConnected() == true after the server closed "
        "the connection).  When the production fix lands, replace "
        "this with `waitFor([&]{ return !m_listener->isConnected(); "
        "}, std::chrono::seconds{5}); TS_ASSERT(!m_listener->"
        "isConnected());`.  Tracked as a separate ticket — see PR "
        "comment 4553042112.",
        m_listener->isConnected());
  }

  void test_LegacyExtractEmptyWorkspace() {
    // The listener cannot complete initWorkspacePart2() until it has
    // received a RunStatusPkt (which is what sets m_dataStartTime and
    // satisfies readyForInitPart2()).  Without it, extractData() blocks
    // for 10 s and returns Exception::NotYet.  Send a NEW_RUN but no
    // event packets so the workspace is initialised with zero events.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                    /*runNum=*/1,
                                    /*pulseId=*/0x0000000100000000ULL),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 3; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    if (ews) {
      TS_ASSERT_EQUALS(ews->getNumberEvents(), 0u);
    }
  }

  void test_LegacyConnectionStatusTransitions() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                    /*runNum=*/1,
                                    /*pulseId=*/0x0000000100000000ULL),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 3; }, std::chrono::seconds{5});
    // After receiving Geometry, BeamlineInfo and a RunStatus the
    // listener has completed initialisation and is Connected.
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
    // scriptIndex >= 5 means the PktWaitForExtract gate has been entered,
    // which guarantees that all four earlier packets (geometry, beamline,
    // NEW_RUN, banked-event) have been pushed onto the wire.  A short
    // sleep then lets the listener's bg thread drain & parse them before
    // extractData() takes the buffer.
    waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    // setRunDetails() stores run_number as a STRING (see
    // SNSLiveEventDataListener.cpp:806 — Strings::toString<int>(...)),
    // so we must request the property as a std::string.
    TS_ASSERT_EQUALS(
        ews->run().getPropertyValueAsType<std::string>("run_number"),
        std::string{"42"});
    TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
  }

  void test_fullRun_beginExtractEndExtract() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 55,
                                    0x0000000100000000ULL),
        // Use the helper-built bank packet with pixel=1 so it matches
        // kMinimalIDF's single-detector panel.  The raw
        // bankedEventPacketV1 fixture references pixel ID 61092 which
        // is not present in kMinimalIDF and would be discarded with
        // an "Invalid pixel ID" warning.
        Testing::buildBankedEventPkt(0x0000000100000000ULL,
                                      /*chargePc=*/1000.0,
                                      {{/*tof=*/100u, /*pixel=*/1u}}),
        Testing::PktWaitForExtract{},      // gate 1 (script index 4)
        Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 55,
                                    0x0000000300000000ULL),
        Testing::PktWaitForExtract{},      // gate 2 (script index 6)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    // First extract: wait for gate 1 to have been entered (scriptIndex
    // becomes 5 once PktWaitForExtract assigns m_scriptIndex = i + 1).
    waitFor([&]{ return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws1 = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::Running);
    m_server->releaseExtractGate(); // release gate 1
    // Second extract: wait for gate 2 to have been entered (scriptIndex
    // becomes 7 after END_RUN has been sent and PktWaitForExtract entered).
    waitFor([&]{ return m_server->scriptIndex() >= 7; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws2 = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate(); // release gate 2
    TS_ASSERT_DIFFERS(ws1, nullptr);
    TS_ASSERT_DIFFERS(ws2, nullptr);
    TS_ASSERT_EQUALS(m_listener->runStatus(),
                     API::ILiveListener::EndRun);
  }

  void test_runNumber_proposalId_title_propagate() {
    // Verifies that run_number (from RunStatusPkt) and proposal_id /
    // run_title (from RunInfoPkt) all propagate into the extracted
    // workspace's Run object.  See Testing::buildRunInfoPkt() in
    // MockSMSServer.cpp for the XML layout expected by
    // SNSLiveEventDataListener::rxPacket(RunInfoPkt).
    const std::string kProposalId = "IPTS-9999";
    const std::string kRunTitle = "integration test run";
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 77,
                                    0x0000000100000000ULL),
        Testing::buildRunInfoPkt(kProposalId, kRunTitle),
        Testing::buildBankedEventPkt(0x0000000100000000ULL,
                                      /*chargePc=*/1000.0,
                                      {{/*tof=*/100u, /*pixel=*/1u}}),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&]{ return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto mws = std::dynamic_pointer_cast<API::MatrixWorkspace>(ws);
    TS_ASSERT_DIFFERS(mws, nullptr);
    if (mws) {
      // run_number is stored as a STRING by setRunDetails() — see
      // SNSLiveEventDataListener.cpp:806.
      TS_ASSERT_EQUALS(
          mws->run().getPropertyValueAsType<std::string>("run_number"),
          std::string{"77"});
      // proposal_id and run_title come from rxPacket(RunInfoPkt) — see
      // SNSLiveEventDataListener.cpp:1190-1267.  Their property names
      // are EXPERIMENT_ID_PROPERTY ("experiment_identifier") and
      // RUN_TITLE_PROPERTY ("run_title").
      TS_ASSERT_EQUALS(
          mws->run().getPropertyValueAsType<std::string>("experiment_identifier"),
          kProposalId);
      TS_ASSERT_EQUALS(
          mws->run().getPropertyValueAsType<std::string>("run_title"),
          kRunTitle);
    }
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
        // A RunStatusPkt is required: it is what sets m_dataStartTime
        // and triggers readyForInitPart2() -> initWorkspacePart2(),
        // which is where the malformed IDF will actually be fed to
        // LoadInstrument and throw the SAXParseException we want to
        // surface through extractData().
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                    /*runNum=*/1,
                                    /*pulseId=*/0x0000000100000000ULL),
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());

    // Wait for the bg thread to have consumed all three packets and
    // attempted initWorkspacePart2() (which must fail and stash
    // m_backgroundException).  We poll for that exception being set by
    // calling extractData() in a tight loop with a short timeout, since
    // the listener does not expose m_backgroundException directly.
    waitFor([&] { return m_server->scriptIndex() >= 3; },
            std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

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
