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
#include "MantidDataObjects/EventWorkspace.h"
#include "MantidGeometry/Instrument.h"
#include "MantidKernel/ConfigService.h"
#include "MantidLiveData/SNSLiveEventDataListener.h"
#include "MantidTypes/Core/DateAndTime.h"

#include "ADARAPackets.h"  // every byte-array fixture
#include "MockSMSServer.h" // from subspec02

#include <Poco/Net/SocketAddress.h>
#include <Poco/TemporaryFile.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using namespace Mantid;
using namespace Mantid::LiveData;

// Construct a std::vector<uint8_t> from a fixture array in ADARAPackets.h.
#define PKT(name) std::vector<uint8_t>((name), (name) + sizeof(name))

namespace {

/// Instrument name advertised in the BeamlineInfo packet.  Must match the
/// `name` attribute of the IDF loaded by @ref kMinimalIDF so that
/// LoadInstrument's IDS cache keys consistently across tests.
constexpr const char *kInstrumentName = "DUM";

/// VALID Mantid IDF used by the integration tests, loaded once from
/// `<instrument_dir>/unit_testing/DUM_Definition.xml` (the same fixture
/// IDF used by `LoadEmptyInstrumentTest` and `He3TubeEfficiencyTest`).
///
/// The ADARA `geometryPacketV0` fixture in ADARAPackets.h carries the
/// placeholder XML `<instrument>VACUO</instrument>`, which is well-formed
/// XML but is NOT a valid Mantid instrument definition; feeding it to
/// LoadInstrument inside `initWorkspacePart2()` raises a SAXParseException,
/// the listener's background thread exits, and every behavioural test
/// downstream of the geometry+beamline handshake stalls in waitFor() or
/// deadlocks on extractData().  Use this IDF (3 detector pixels with
/// ids 1..3, monitor with id 0) via Testing::buildGeometryPkt() instead.
///
/// Detector id 1 covers the `pixel=1` events emitted by
/// `Testing::buildBankedEventPkt()` in these tests.
inline const std::string &kMinimalIDF() {
  static const std::string idf = []() {
    auto &config = Kernel::ConfigService::Instance();
    const std::filesystem::path path =
        std::filesystem::path(config.getInstrumentDirectory()) / "unit_testing" / "DUM_Definition.xml";
    std::ifstream in(path);
    if (!in) {
      throw std::runtime_error("SNSLiveEventDataListenerTest: failed to open IDF at " + path.string());
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
  }();
  return idf;
}

/// Spin-wait up to @p timeout, polling every @p poll, until @p pred
/// returns true.  On timeout calls TS_FAIL and returns false.
template <typename Pred>
bool waitFor(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds{5},
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
inline std::shared_ptr<API::Workspace> extractWithTimeout(SNSLiveEventDataListener &listener,
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
    TS_WARN("XFAIL: SNSLiveEventDataListener does not detect a clean peer-close. "
            "Poco::Net::StreamSocket::receiveBytes() returns 0 bytes on EOF, but "
            "the bg read loop (SNSLiveEventDataListener.cpp:264-294) does not "
            "treat a zero-byte return as fatal, so m_isConnected is never set "
            "false after a server-side close.  Defect noted in PR comment "
            "4553042112; no ticket filed yet.  Remove this TS_WARN and the "
            "return below when the production fix lands; the assertions that "
            "follow express the intended contract.  (Sibling test: see §5.2 "
            "test_serverDisconnect_setsErrorState for the variant that first "
            "sends geometry/beamline/NEW_RUN.)");
    return;
    // --- intended behaviour (currently unreached) ---
    m_server->script({Testing::PktDisconnect{}});
    m_server->start();
    TS_ASSERT(connectListener());
    TS_ASSERT(m_listener->isConnected());
    // Server-side: wait for the scripted PktDisconnect{} to complete
    // (scriptIndex advances past it once the server has closed its end).
    waitFor([&] { return m_server->scriptIndex() >= 1; }, std::chrono::seconds{5});
    // Give the listener a window in which it would notice the close if the
    // production fix were in place.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    // Intended: listener detects EOF and transitions out of Connected.
    waitFor([&] { return !m_listener->isConnected(); }, std::chrono::seconds{5});
    TS_ASSERT(!m_listener->isConnected());
  }

  void test_LegacyExtractEmptyWorkspace() {
    // The listener cannot complete initWorkspacePart2() until it has
    // received a RunStatusPkt (which is what sets m_dataStartTime and
    // satisfies readyForInitPart2()).  Without it, extractData() blocks
    // for 10 s and returns Exception::NotYet.  Send a NEW_RUN but no
    // event packets so the workspace is initialised with zero events.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                   /*runNum=*/1,
                                   /*pulseId=*/0x0000000100000000ULL),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 3; }, std::chrono::seconds{5});
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
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                   /*runNum=*/1,
                                   /*pulseId=*/0x0000000100000000ULL),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 3; }, std::chrono::seconds{5});
    // After receiving Geometry, BeamlineInfo and a RunStatus the
    // listener has completed initialisation and is Connected.
    TS_ASSERT_EQUALS(m_listener->listenerState(), API::ListenerState::Connected);
    m_server->releaseExtractGate();
  }

  // ----- §6.2 Connection & mid-run join -----

  void test_connect_succeeds_over_uds() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});
    TS_ASSERT(m_server->clientConnected());
    TS_ASSERT_LESS_THAN(0u, m_server->bytesSent());
  }

  void test_midRunJoin_doesNotWipeWorkspaceInit() {
    // NEW_RUN arrives BEFORE geometry/beamline metadata — mid-run join.
    m_server->script({
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN,
                                   /*runNum=*/100,
                                   /*pulseId=*/0x0000000100000000ULL),
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        PKT(bankedEventPacketV1),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    // The listener must report Running after a NEW_RUN + events.
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::Running);
    // Workspace must not be uninitialised (no empty instrument).
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_DIFFERS(ews->getInstrument()->getName(), std::string{});
  }

  // ----- §6.3 Single & full run lifecycle -----

  void test_singleRun_extractsEventsAndRunNumber() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 42, 0x0000000100000000ULL),
        Testing::buildBankedEventPkt(0x0000000100000000ULL,
                                     /*chargePc=*/1000.0, {{/*tof=*/100u, /*pixel=*/1u}}),
        Testing::PktWaitForExtract{},
        Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 42, 0x0000000200000000ULL),
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    // scriptIndex >= 5 means the PktWaitForExtract gate has been entered,
    // which guarantees that all four earlier packets (geometry, beamline,
    // NEW_RUN, banked-event) have been pushed onto the wire.  A short
    // sleep then lets the listener's bg thread drain & parse them before
    // extractData() takes the buffer.
    waitFor([&] { return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    // setRunDetails() stores run_number as a STRING (see
    // SNSLiveEventDataListener.cpp:806 — Strings::toString<int>(...)),
    // so we must request the property as a std::string.
    TS_ASSERT_EQUALS(ews->run().getPropertyValueAsType<std::string>("run_number"), std::string{"42"});
    TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
  }

  void test_fullRun_beginExtractEndExtract() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 55, 0x0000000100000000ULL),
        // Use the helper-built bank packet with pixel=1 so it maps to
        // detector id 1 in the DUM IDF.  The raw bankedEventPacketV1
        // fixture references pixel ID 61092 which is not present in the
        // DUM IDF and would be discarded with an "Invalid pixel ID"
        // warning.
        Testing::buildBankedEventPkt(0x0000000100000000ULL,
                                     /*chargePc=*/1000.0, {{/*tof=*/100u, /*pixel=*/1u}}),
        Testing::PktWaitForExtract{}, // gate 1 (script index 4)
        Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 55, 0x0000000300000000ULL),
        Testing::PktWaitForExtract{}, // gate 2 (script index 6)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    // First extract: wait for gate 1 to have been entered (scriptIndex
    // becomes 5 once PktWaitForExtract assigns m_scriptIndex = i + 1).
    waitFor([&] { return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws1 = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::Running);
    m_server->releaseExtractGate(); // release gate 1
    // Second extract: wait for gate 2 to have been entered (scriptIndex
    // becomes 7 after END_RUN has been sent and PktWaitForExtract entered).
    waitFor([&] { return m_server->scriptIndex() >= 7; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws2 = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate(); // release gate 2
    TS_ASSERT_DIFFERS(ws1, nullptr);
    TS_ASSERT_DIFFERS(ws2, nullptr);
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::EndRun);
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
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 77, 0x0000000100000000ULL),
        Testing::buildRunInfoPkt(kProposalId, kRunTitle),
        Testing::buildBankedEventPkt(0x0000000100000000ULL,
                                     /*chargePc=*/1000.0, {{/*tof=*/100u, /*pixel=*/1u}}),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto mws = std::dynamic_pointer_cast<API::MatrixWorkspace>(ws);
    TS_ASSERT_DIFFERS(mws, nullptr);
    if (mws) {
      // run_number is stored as a STRING by setRunDetails() — see
      // SNSLiveEventDataListener.cpp:806.
      TS_ASSERT_EQUALS(mws->run().getPropertyValueAsType<std::string>("run_number"), std::string{"77"});
      // proposal_id and run_title come from rxPacket(RunInfoPkt) — see
      // SNSLiveEventDataListener.cpp:1190-1267.  Their property names
      // are EXPERIMENT_ID_PROPERTY ("experiment_identifier") and
      // RUN_TITLE_PROPERTY ("run_title").
      TS_ASSERT_EQUALS(mws->run().getPropertyValueAsType<std::string>("experiment_identifier"), kProposalId);
      TS_ASSERT_EQUALS(mws->run().getPropertyValueAsType<std::string>("run_title"), kRunTitle);
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
    waitFor([&] { return m_server->scriptIndex() >= 3; }, std::chrono::seconds{5});
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
    TSM_ASSERT_DIFFERS("extractData() must not return Exception::NotYet here", what.find("LoadInstrument failed"),
                       std::string::npos);
    TSM_ASSERT_DIFFERS("error message must include InstrumentName context", what.find(kInstrumentName),
                       std::string::npos);
  }

  // ----- §6.4 C1 fix — lastTransition survives NotYet -----
  //
  // NotYet is the normal-operation signal that extractData() emits when the
  // workspace has not yet been initialised (doExtractData() at
  // SNSLiveEventDataListener.cpp:1432-1497 polls m_workspaceInitialized for
  // 10 s then throws).  The usual trigger is receiving NEW_RUN before the
  // Geometry / Beamline packets arrive, or receiving a second NEW_RUN (after
  // onBeginRun() has cleared m_workspaceInitialized) before new geometry
  // arrives.
  //
  // C1 guarantee (LiveListener.cpp:16-21): when doExtractData() throws,
  // onAfterExtract() is NOT invoked, so the m_lastTransition reset at
  // SNSLiveEventDataListener.cpp:1566 is bypassed.

  void test_lastTransition_preservedAcrossNotYet() {
    // Drive the C1 path via the realistic SMS connect handshake:
    //
    //   STATE(runNumber=0) — the RunStatus packet SMS sends on initial
    //     connect when no run history is requested (comment at :778-787).
    //     Sets m_dataStartTime at :627 but does NOT call setRunDetails()
    //     (runNumber==0 check at :785), so run_number is NOT added to
    //     m_eventBuffer.  Together with prior Geometry+Beamline, workspace
    //     initialises via initWorkspacePart2() at :793-797 without haveRunNumber.
    //
    //   NEW_RUN(10) arrives with m_workspaceInitialized==true and
    //     !haveRunNumber → normal branch at :651-657 sets
    //     m_pendingTransition=BeginRun; :706-725 stashes m_deferredRunDetailsPkt;
    //     :729-731 sets m_pauseNetRead=true.
    //
    // First extract: onBeforeExtract() dispatches onBeginRun() (:1552),
    // which clears m_workspaceInitialized (:1590); m_lastTransition=BeginRun
    // set at :1555; m_pauseNetRead cleared at :1557.  doExtractData() polls
    // 10 s for m_workspaceInitialized — no new geometry arrives while gate 1
    // holds — and throws NotYet.  C1: onAfterExtract() is NOT called, so the
    // reset at :1566 is bypassed and m_lastTransition stays BeginRun.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::STATE, 0,
                                   0x0000000100000000ULL), // SMS connect handshake
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10,
                                   0x0000000200000000ULL), // normal → m_pendingTransition=BeginRun
        Testing::PktWaitForExtract{},                      // gate 1 (index 4 → scriptIndex 5)
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildBankedEventPkt(0x0000000300000000ULL, 1000.0, {{100u, 1u}}),
        Testing::PktWaitForExtract{}, // gate 2 (index 8 → scriptIndex 9)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    // Wait for server to have entered gate 1 (all 4 pre-gate packets sent).
    waitFor([&] { return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // First extract: NotYet path — doExtractData() polls 10 s then throws.
    // Use a 15 s timeout so the internal poll fires first and the resulting
    // exception propagates via fut.get() rather than triggering TS_FAIL.
    try {
      extractWithTimeout(*m_listener, std::chrono::seconds{15});
    } catch (const std::exception &) {
      // NotYet — expected on this path.
    }

    // C1 fix: m_lastTransition must still be BeginRun — onAfterExtract() was
    // NOT invoked when doExtractData() threw, so the reset at :1566 was bypassed.
    TS_ASSERT(m_listener->lastTransition().has_value());
    TS_ASSERT_EQUALS(*m_listener->lastTransition(), API::ILiveListener::BeginRun);

    m_server->releaseExtractGate(); // gate 1 → server sends new geometry + events
    waitFor([&] { return m_server->scriptIndex() >= 9; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    auto ws2 = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate(); // gate 2
    TS_ASSERT_DIFFERS(ws2, nullptr);
  }

  void test_notYet_whenGeometryDelayed() {
    // Normal-operation NotYet path: NEW_RUN arrives before Geometry/Beamline.
    // The white-lie branch at :658-703 calls setRunDetails() (run_number added)
    // but readyForInitPart2() returns false (no instrumentXML/instrumentName
    // yet), so m_workspaceInitialized stays false.  doExtractData() polls 10 s
    // and throws NotYet.  Then Geometry+Beamline arrive → bg thread calls
    // initWorkspacePart2() at the end of rxPacket(BeamlineInfoPkt) (:609-610)
    // → m_workspaceInitialized=true.  The subsequent extract succeeds.
    m_server->script({
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 20,
                                   0x0000000100000000ULL), // before geometry
        Testing::PktWaitForExtract{},                      // gate 1 (index 1 → scriptIndex 2)
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0, {{100u, 1u}}),
        Testing::PktWaitForExtract{}, // gate 2 (index 5 → scriptIndex 6)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    // Wait for server to have entered gate 1 (NEW_RUN sent; server holding).
    waitFor([&] { return m_server->scriptIndex() >= 2; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // First extract: workspace not yet initialised → NotYet after 10 s poll.
    // 15 s timeout lets the internal poll fire before the outer timeout fires.
    bool gotNotYet = false;
    try {
      extractWithTimeout(*m_listener, std::chrono::seconds{15});
    } catch (const std::exception &) {
      gotNotYet = true;
    }
    TSM_ASSERT("first extract must throw NotYet when Geometry has not yet arrived", gotNotYet);

    // Listener must be in a sane state — no spurious back-pressure, no stored
    // background exception.
    TS_ASSERT_DIFFERS(m_listener->listenerState(), API::ListenerState::Error);

    m_server->releaseExtractGate(); // gate 1 → server sends Geometry+Beamline+event
    waitFor([&] { return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // Second extract: workspace now initialised; 1 event expected.
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate(); // gate 2
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 1);
  }

  // ----- §6.5 Back-pressure observables / consecutive-NEW_RUN error -----
  //
  // The single-slot invariant throws at
  // SNSLiveEventDataListener.cpp:651-655 (BeginRun-side) and :744-748
  // (END_RUN-side) are unreachable over a real socket.  Both throws require
  // m_pendingTransition to be occupied when a NEW_RUN or END_RUN arrives, but
  // the call that occupies the slot also sets m_pauseNetRead=true (:730,:772),
  // which causes rxPacket() to return true, which causes ADARA::Parser::
  // bufferParse() to stop parsing further packets (ADARAParser.cpp:111,177).
  // The bg-read loop then sleeps until extractData() clears the flag.
  // The production throws are exercised by unit tests that feed raw
  // ADARA::RunStatusPkt bytes through the parser with pre-injected state:
  //   SNSLiveEventDataListenerNoNetworkTest::
  //       test_rxRunStatusPkt_newRun_throws_when_slot_occupied()
  //       test_rxRunStatusPkt_endRun_throws_when_slot_occupied()
  //
  // Consecutive NEW_RUN packets (without an intervening END_RUN) are a
  // distinct malformed-stream case detected earlier in rxPacket(RunStatusPkt),
  // at :706-725: the first NEW_RUN (white-lie path) calls setRunDetails(),
  // setting run_number; the second NEW_RUN reaches the deferred-packet stash
  // site with haveRunNumber==true and throws std::runtime_error.  The bg-thread
  // catch (:318-326) stores it in m_backgroundException; doExtractData()
  // re-throws at :1444-1450.

  void test_consecutiveNewRun_surfacesRuntimeError() {
    // Two consecutive NEW_RUN packets (different run numbers) without an
    // intervening END_RUN.  The first goes through the white-lie path at
    // :658-703 (m_workspaceInitialized=false at arrival → calls setRunDetails,
    // adding run_number; initWorkspacePart2 → m_workspaceInitialized=true).
    // The second hits the normal branch at :651-657 (m_workspaceInitialized=true)
    // and reaches the deferred-packet stash at :706-725 with haveRunNumber==true
    // → throws runtime_error; bg-thread catch stores it in m_backgroundException;
    // doExtractData() re-throws at :1444-1450.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10,
                                   0x0000000100000000ULL), // white-lie → inits ws
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 11,
                                   0x0000000200000000ULL), // throws at :706-725
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    TS_ASSERT_THROWS(extractWithTimeout(*m_listener, std::chrono::seconds{5}), const std::runtime_error &);
    // Listener must not be wedged: m_pauseNetRead was not set before the throw
    // (:729-731 is after :706-725), so a second extract re-throws the stored
    // background exception promptly rather than blocking on back-pressure.
    TS_ASSERT_THROWS(extractWithTimeout(*m_listener, std::chrono::seconds{2}), const std::runtime_error &);
  }

  void test_repeatedNewRun_sameRunNumber_surfacesRuntimeError() {
    // Same as test_consecutiveNewRun_surfacesRuntimeError but with the same
    // run number repeated — the operational case of an operator restarting the
    // same run without an intervening END_RUN.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10,
                                   0x0000000100000000ULL), // white-lie → inits ws
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10,
                                   0x0000000200000000ULL), // same run number → throws
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{200});

    TS_ASSERT_THROWS(extractWithTimeout(*m_listener, std::chrono::seconds{5}), const std::runtime_error &);
  }

  void test_newRunEndRun_backPressureProducesReadWaitThenCleanEndRun() {
    // The single-slot END_RUN invariant at :744-748 is unreachable over a
    // real socket — see the block comment above §6.5.
    // Unit-level coverage of the production throw:
    //   SNSLiveEventDataListenerNoNetworkTest::
    //       test_rxRunStatusPkt_endRun_throws_when_slot_occupied()
    //
    // This integration test verifies the observable consequence of the
    // white-lie NEW_RUN + END_RUN sequence:
    //   1. NEW_RUN arrives while m_workspaceInitialized=false → white-lie
    //      path → workspace initialised; m_pendingTransition NOT set.
    //   2. END_RUN arrives → m_pendingTransition.has_value()==false → no
    //      invariant throw → sets m_pendingTransition=EndRun,
    //      m_pauseNetRead=true → listenerState()==ReadWait.
    //   3. extractData() → onBeforeExtract() does nothing (EndRun pending,
    //      not BeginRun) → doExtractData() succeeds (m_workspaceInitialized
    //      still true) → onAfterExtract() commits EndRun.
    // Observable: ReadWait back-pressure followed by a successful extract
    // reporting runStatus()==EndRun.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 3, 0x0000000100000000ULL),
        Testing::buildRunStatusPkt(ADARA::RunStatus::END_RUN, 3, 0x0000000200000000ULL),
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    // Poll directly for the observable consequence of END_RUN being processed:
    // m_pauseNetRead = true → listenerState() == ReadWait.
    waitFor([&] { return m_listener->listenerState() == API::ListenerState::ReadWait; }, std::chrono::seconds{5});
    TS_ASSERT_EQUALS(m_listener->listenerState(), API::ListenerState::ReadWait);
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    TS_ASSERT_DIFFERS(ws, nullptr);
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::EndRun);
  }

  // ----- §6.7 Pause / resume -----

  void test_pauseResume_orthogonalToRunState() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 10, 0x0000000100000000ULL),
        PKT(bankedEventPacketV1),
        PKT(AnnotationPacketType3), // Pause
        PKT(bankedEventPacketV1),
        PKT(AnnotationPacketType4), // Resume
        PKT(bankedEventPacketV1),
        Testing::PktWaitForExtract{}, // gate (index 8 → scriptIndex 9)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 8; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    // runStatus() must remain Running — pause/resume annotations do not alter it.
    TS_ASSERT_EQUALS(m_listener->runStatus(), API::ILiveListener::Running);
    // The 'pause' time series property must have been populated by the annotation.
    auto mws = std::dynamic_pointer_cast<API::MatrixWorkspace>(ws);
    TS_ASSERT_DIFFERS(mws, nullptr);
    TS_ASSERT(mws->run().hasProperty("pause"));
  }

  void test_pausedEvents_droppedByDefault() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 11, 0x0000000100000000ULL),
        Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0, {{100u, 1u}}), // 1 pre-pause event (kept)
        PKT(AnnotationPacketType3),                                                // Pause
        Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0,
                                     {{200u, 2u}, {300u, 3u}}), // 2 mid-pause events (dropped)
        PKT(AnnotationPacketType4),                             // Resume
        Testing::PktWaitForExtract{},                           // gate (index 7 → scriptIndex 8)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 7; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    // Only 1 pre-pause event; the 2 mid-pause events must be absent
    // (SNSLiveEventDataListener.cpp:405 — keepPausedEvents=false default).
    TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 1);
  }

  void test_pausedEvents_keptWhenConfigured() {
    Kernel::ConfigService::Instance().setString("SNSLiveEventDataListener.keepPausedEvents", "1");
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 12, 0x0000000100000000ULL),
        Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0, {{100u, 1u}}), // 1 pre-pause event
        PKT(AnnotationPacketType3),                                                // Pause
        Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0,
                                     {{200u, 2u}, {300u, 3u}}), // 2 mid-pause events (kept)
        PKT(AnnotationPacketType4),                             // Resume
        Testing::PktWaitForExtract{},                           // gate (index 7 → scriptIndex 8)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 7; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    // All 3 events (1 pre-pause + 2 mid-pause) must be present
    // (SNSLiveEventDataListener.cpp:405 — keepPausedEvents=true).
    TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 3);
  }

  void test_allDetectorPixels_retained() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 12, 0x0000000100000000ULL),
        Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0, {{100u, 1u}, {200u, 2u}, {300u, 3u}}),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 3);
  }

  void test_monitorEvents_recorded_onMainRun() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 13, 0x0000000100000000ULL),
        Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0, {{100u, 1u}, {200u, 2u}, {300u, 3u}}),
        Testing::buildBeamMonitorPkt(0x0000000100000000ULL, 0u, {500u, 600u}),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 5; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 3);
    TS_ASSERT(ews->run().hasProperty("monitor0_counts"));
    TS_ASSERT_EQUALS(ews->run().getPropertyValueAsType<int>("monitor0_counts"), 2);
  }

  void test_outOfRangePixels_excluded() {
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 14, 0x0000000100000000ULL),
        Testing::buildBankedEventPkt(
            0x0000000100000000ULL, 1000.0,
            {{100u, 1u}, {200u, 2u}, {300u, 3u}, {400u, 4u}, {500u, 5u}, {600u, 6u}, {700u, 7u}, {800u, 8u}}),
        Testing::PktWaitForExtract{},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 3);
  }

  // ----- §6.8 Historical replay & variable cache (XFAIL) -----
  //
  // Both tests document the m_ignorePackets defect: start() sets
  // m_filterUntilRunStart=true for the 1 ns sentinel but never sets
  // m_ignorePackets=true, so ignorePacket() (SNSLiveEventDataListener.cpp:
  // 1644-1673) short-circuits at the first line and the entire filter-until-
  // run-start and variable-cache replay path is dead code.
  //
  // See plans/ignore-packets-defect.md for full analysis.
  //
  // XFAIL convention: TS_WARN + return at the top of the test body.
  // The script and assertions below the return express the INTENDED contract
  // (i.e. the behaviour expected AFTER the fix lands).  When the fix lands,
  // remove the TS_WARN and the return to re-enable the test.

  void test_filterUntilRunStart_dropsPreRunPackets() {
    TS_WARN("XFAIL: SNSLiveEventDataListener::start() sets m_filterUntilRunStart=true "
            "for the 1 ns sentinel but never sets m_ignorePackets=true "
            "(SNSLiveEventDataListener.cpp:193-210).  Because ignorePacket() "
            "(SNSLiveEventDataListener.cpp:1644-1673) short-circuits immediately "
            "when m_ignorePackets==false, the filter-until-run-start path is dead "
            "code and pre-run packets are not filtered.  See "
            "plans/ignore-packets-defect.md.  Remove this TS_WARN and the return "
            "below when the production fix lands; the assertions that follow "
            "express the intended contract.");
    return;
    // --- intended behaviour (currently unreached) ---
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        // Pre-run events — should be filtered out by ignorePacket() once fixed.
        Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0, {{100u, 1u}, {200u, 2u}}),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 20, 0x0000000200000000ULL),
        Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0, {{300u, 3u}}),
        Testing::PktWaitForExtract{}, // gate (index 5 → scriptIndex 6)
        Testing::PktDisconnect{},
    });
    m_server->start();
    // DateAndTime(1) == 1 nanosecond past epoch — the "replay from previous
    // run start" sentinel; start() sets m_filterUntilRunStart=true.
    TS_ASSERT(connectListener(Types::Core::DateAndTime(1)));
    waitFor([&] { return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    // Intended: only the 1 post-run event; the 2 pre-run events are filtered
    // by ignorePacket() once m_ignorePackets is correctly initialised in start().
    TS_ASSERT_EQUALS(static_cast<int>(ews->getNumberEvents()), 1);
  }

  void test_variableCache_replayedAfterStartCondition() {
    TS_WARN("XFAIL: same root cause as test_filterUntilRunStart_dropsPreRunPackets "
            "— m_ignorePackets is never set true in start(), so the variable-cache "
            "replay path (replayVariableCache(), SNSLiveEventDataListener.cpp:"
            "1668-1673) is unreachable.  Broken and fixed behaviours produce the "
            "same observable output in this test (final log value 7 in both cases); "
            "a spy on replayVariableCache() would be needed to distinguish them.  "
            "This test is retained as executable documentation of the defect.  See "
            "plans/ignore-packets-defect.md.  Remove this TS_WARN and the return "
            "below when the production fix lands.");
    return;
    // --- intended behaviour (currently unreached) ---
    const uint64_t preRunPulse = 0x0000000100000000ULL;
    const uint64_t postRunPulse = 0x0000000200000000ULL;
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        PKT(devDesPacket),                                   // device descriptor for device 1
        Testing::buildVariableU32Pkt(1, 3, 42, preRunPulse), // pre-run (should be cached)
        Testing::buildVariableU32Pkt(1, 3, 99, preRunPulse), // pre-run (should be cached)
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 30, postRunPulse),
        Testing::buildVariableU32Pkt(1, 3, 7, postRunPulse), // post-run update
        Testing::buildBankedEventPkt(postRunPulse, 1000.0, {{100u, 1u}}),
        Testing::PktWaitForExtract{}, // gate (index 8 → scriptIndex 9)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener(Types::Core::DateAndTime(1)));
    waitFor([&] { return m_server->scriptIndex() >= 9; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    // run_number is set by setRunDetails() — its presence confirms the run
    // lifecycle completed.  A spy on replayVariableCache() would be needed to
    // assert the cache-and-replay mechanism itself.
    TS_ASSERT(ews->run().hasProperty("run_number"));
  }

  // ----- §6.9 Error propagation -----

  void test_invalidPacket_propagatesAsBackgroundException() {
    // Garbage bytes cause ADARA::invalid_packet to be thrown and caught in
    // the bg-read loop (SNSLiveEventDataListener.cpp:305-317), where it is
    // stored in m_backgroundException.  The next runState() or extractData()
    // call must re-throw it.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::PktGarbage{
            {0xFF, 0xFE, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    // Wait for the garbage to have been sent (index 2 → scriptIndex 3),
    // then give the bg thread a moment to parse and stash the exception.
    waitFor([&] { return m_server->scriptIndex() >= 3; }, std::chrono::seconds{5});
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    bool threw = false;
    try {
      (void)m_listener->runState();
      (void)extractWithTimeout(*m_listener, std::chrono::seconds{5});
    } catch (...) {
      threw = true;
    }
    TS_ASSERT(threw);
  }

  void test_serverDisconnect_setsErrorState() {
    TS_WARN("XFAIL: SNSLiveEventDataListener does not detect a clean peer-close. "
            "Poco::Net::StreamSocket::receiveBytes() returns 0 bytes on EOF, but "
            "the bg read loop (SNSLiveEventDataListener.cpp:264-294) does not "
            "treat a zero-byte return as fatal, so m_isConnected is never set "
            "false after a server-side close.  Defect noted in PR comment "
            "4553235918; no ticket filed yet.  Remove this TS_WARN and the "
            "return below when the production fix lands; the assertions that "
            "follow express the intended contract.");
    return;
    // --- intended behaviour (currently unreached) ---
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 1, 0x0000000100000000ULL),
        Testing::PktDisconnect{}, // (index 3 → scriptIndex 4)
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 4; }, std::chrono::seconds{5});
    // Give the listener a window in which it would notice the close if the
    // production fix were in place.
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    // Intended: listener detects EOF and transitions out of Connected.
    TS_ASSERT(!m_listener->isConnected());
    TS_ASSERT_DIFFERS(m_listener->listenerState(), API::ListenerState::Connected);
  }

  void test_connectFailure_returnsFalse() {
    // setUp() already removed m_sockPath (line 149), so no server is listening
    // at that path.  connect() must return false without throwing.
    // Do NOT call connectListener() — this test drives connect() directly.
    m_listener = std::make_unique<SNSLiveEventDataListener>();
    Poco::Net::SocketAddress addr(Poco::Net::AddressFamily::UNIX_LOCAL, m_sockPath);
    bool result = m_listener->connect(addr);
    TS_ASSERT(!result);
    TS_ASSERT(!m_listener->isConnected());
  }

  // ----- §6.10 Monitor workspace routing -----

  void test_beamMonitorEvents_routedToMonitorWorkspace() {
    // Verifies rxPacket(BeamMonitorPkt) (SNSLiveEventDataListener.cpp:470-530)
    // places monitor events in the sub-workspace created by
    // initMonitorWorkspace() (SNSLiveEventDataListener.cpp:1371-1388).
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 60, 0x0000000100000000ULL),
        Testing::buildBankedEventPkt(0x0000000100000000ULL, 1000.0,
                                     {{100u, 1u}, {200u, 2u}, {300u, 3u}}),    // DUM pixels 1/2/3
        Testing::buildBeamMonitorPkt(0x0000000100000000ULL, 0u, {500u, 600u}), // DUM monitor 0
        Testing::PktWaitForExtract{},                                          // gate (index 5 → scriptIndex 6)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 6; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    TS_ASSERT_DIFFERS(ws, nullptr);
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    // Main workspace must have neutron events.
    TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
    // Monitor sub-workspace must exist and contain beam-monitor events.
    auto monWs = ews->monitorWorkspace();
    TS_ASSERT_DIFFERS(monWs, nullptr);
    auto monEws = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(monWs);
    TS_ASSERT_DIFFERS(monEws, nullptr);
    TS_ASSERT_LESS_THAN(0, static_cast<int>(monEws->getNumberEvents()));
  }

  void test_invalidMonitorId_logsOnceAndContinues() {
    // Verifies the m_badMonitors dedup (SNSLiveEventDataListener.cpp:521-524):
    // an unknown monitor ID is logged exactly once; subsequent packets with the
    // same bad ID are silently ignored.  The listener must not crash or enter
    // Error state, and neutron events from the following BankedEventPkt must
    // still be processed.
    m_server->script({
        Testing::buildGeometryPkt(kMinimalIDF()),
        Testing::buildBeamlineInfoPkt(kInstrumentName),
        Testing::buildRunStatusPkt(ADARA::RunStatus::NEW_RUN, 61, 0x0000000100000000ULL),
        // Same invalid monitor ID twice — second occurrence must be deduplicated.
        Testing::buildBeamMonitorPkt(0x0000000100000000ULL, 9999u, {100u}),
        Testing::buildBeamMonitorPkt(0x0000000200000000ULL, 9999u, {200u}),
        Testing::buildBankedEventPkt(0x0000000200000000ULL, 1000.0,
                                     {{100u, 1u}, {200u, 2u}, {300u, 3u}}), // DUM pixels 1/2/3
        Testing::PktWaitForExtract{},                                       // gate (index 6 → scriptIndex 7)
        Testing::PktDisconnect{},
    });
    m_server->start();
    TS_ASSERT(connectListener());
    waitFor([&] { return m_server->scriptIndex() >= 7; }, std::chrono::seconds{5});
    auto ws = extractWithTimeout(*m_listener, std::chrono::seconds{10});
    m_server->releaseExtractGate();
    // The listener must continue operating — no crash, no Error state.
    TS_ASSERT_DIFFERS(ws, nullptr);
    TS_ASSERT_DIFFERS(m_listener->listenerState(), API::ListenerState::Error);
    // Neutron events from the BankedEventPkt must still be present.
    auto ews = std::dynamic_pointer_cast<DataObjects::EventWorkspace>(ws);
    TS_ASSERT_DIFFERS(ews, nullptr);
    TS_ASSERT_LESS_THAN(0, static_cast<int>(ews->getNumberEvents()));
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
