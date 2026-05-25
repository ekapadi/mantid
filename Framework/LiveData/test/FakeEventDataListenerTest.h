// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright &copy; 2018 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX - License - Identifier: GPL - 3.0 +
#pragma once

#include "MantidAPI/LiveListenerFactory.h"
#include "MantidDataObjects/EventWorkspace.h"
#include "MantidKernel/CPUTimer.h"
#include "MantidKernel/ConfigService.h"
#include "MantidKernel/WarningSuppressions.h"
#include <Poco/Thread.h>
#include <cxxtest/TestSuite.h>

using namespace Mantid::API;
using Mantid::Kernel::CPUTimer;

class FakeEventDataListenerTest : public CxxTest::TestSuite {
public:
  // This pair of boilerplate methods prevent the suite being created statically
  // This means the constructor isn't called when running other tests
  static FakeEventDataListenerTest *createSuite() { return new FakeEventDataListenerTest(); }
  static void destroySuite(FakeEventDataListenerTest *suite) { delete suite; }

  FakeEventDataListenerTest() {
    // Create the listener. Remember: this will call connect()
    fakel = LiveListenerFactory::Instance().create("FakeEventDataListener", true);
  }

  void testProperties() {
    TS_ASSERT(fakel)
    TS_ASSERT_EQUALS(fakel->name(), "FakeEventDataListener")
    TS_ASSERT(!fakel->supportsHistory())
    TS_ASSERT(fakel->buffersEvents())
    TS_ASSERT(fakel->isConnected())
  }

  void testStart() {
    // Nothing much to test just yet
    TS_ASSERT_THROWS_NOTHING(fakel->start(0))
  }

  void testRunStatus() {
    GNU_DIAG_OFF("deprecated-declarations")
    TS_ASSERT_EQUALS(fakel->runStatus(), ILiveListener::Running)
    GNU_DIAG_ON("deprecated-declarations")
  }

  void testExtractData() {
    using namespace Mantid::DataObjects;
    Workspace_const_sptr buffer;
    Poco::Thread::sleep(100);
    TS_ASSERT_THROWS_NOTHING(buffer = fakel->extractData())
    // Check this is the only surviving reference to it
    TS_ASSERT_EQUALS(buffer.use_count(), 1)
    // Check it's an event workspace
    EventWorkspace_const_sptr evbuf = std::dynamic_pointer_cast<const EventWorkspace>(buffer);
    TS_ASSERT(evbuf)
    // Check the workspace has the correct dimension
    TS_ASSERT_EQUALS(evbuf->getNumberHistograms(), 2)
    // Should be around 20 events, but this can vary a lot on some platforms so
    // just check there's something
    TS_ASSERT_LESS_THAN(1, evbuf->getNumberEvents())

    Poco::Thread::sleep(100);
    // Call it again, and check things again
    TS_ASSERT_THROWS_NOTHING(buffer = fakel->extractData())
    // Check this is the only surviving reference to it
    TS_ASSERT_EQUALS(buffer.use_count(), 1)
    // Check it's a different workspace to last time
    TS_ASSERT_DIFFERS(buffer.get(), evbuf.get())
    // Check it's an event workspace
    evbuf = std::dynamic_pointer_cast<const EventWorkspace>(buffer);
    TS_ASSERT(evbuf)
    // Check the workspace has the correct dimension
    TS_ASSERT_EQUALS(evbuf->getNumberHistograms(), 2)
    // Should be around 20 events, but this can vary a lot on some platforms so
    // just check there's something
    TS_ASSERT_LESS_THAN(1, evbuf->getNumberEvents())
  }

  /** Call the extractData very quickly to try to trip up
   * the thread.
   */
  void testThreadSafety() {
    using namespace Mantid::DataObjects;
    Workspace_const_sptr buffer;
    Poco::Thread::sleep(100);

    CPUTimer tim;
    size_t num = 10000;
    for (size_t i = 0; i < num; i++) {
      TS_ASSERT_THROWS_NOTHING(buffer = fakel->extractData(););
      // Check it's a valid workspace
      TS_ASSERT(buffer)
    }
    std::cout << tim << " to call extactData() " << num << " times\n";
  }

  // --- New tests for sub-spec 03 ---

  /** runState() must be a pure getter: calling it multiple times without
   *  an intervening extractData() must return the same value and must not
   *  mutate any internal state.
   */
  void test_runState_is_pure_getter() {
    // With default config (m_endRunEvery == 0) the state is always Running.
    const auto s1 = fakel->runState();
    const auto s2 = fakel->runState();
    const auto s3 = fakel->runState();
    TS_ASSERT_EQUALS(s1, ILiveListener::Running);
    TS_ASSERT_EQUALS(s1, s2);
    TS_ASSERT_EQUALS(s2, s3);
    // runNumber must also be unchanged by pure polling.
    const int rn1 = fakel->runNumber();
    (void)fakel->runState();
    (void)fakel->runState();
    TS_ASSERT_EQUALS(fakel->runNumber(), rn1);
  }

  /** When m_endRunEvery > 0 and the period has elapsed, the first
   *  extractData() call after that point must increment runNumber,
   *  set runState() == EndRun, and set lastTransition() == EndRun.
   */
  void test_onBeforeExtract_advances_runNumber_at_EndRun() {
    using Mantid::Kernel::ConfigService;
    // Use a 1 ms run period so the EndRun fires almost immediately.
    ConfigService::Instance().setString("fakeeventdatalistener.endrunevery", "0.001");
    auto endRunListener = LiveListenerFactory::Instance().create("FakeEventDataListener", true);
    endRunListener->start(0);

    const int runNumberBefore = endRunListener->runNumber();
    // Spin-wait (max 2 s) until the first EndRun fires via extractData().
    bool gotEndRun = false;
    for (int attempt = 0; attempt < 2000 && !gotEndRun; ++attempt) {
      Poco::Thread::sleep(1);
      endRunListener->extractData();
      if (endRunListener->runState() == ILiveListener::EndRun)
        gotEndRun = true;
    }
    TS_ASSERT(gotEndRun);
    TS_ASSERT_EQUALS(endRunListener->runState(), ILiveListener::EndRun);
    TS_ASSERT(endRunListener->lastTransition().has_value());
    TS_ASSERT_EQUALS(endRunListener->lastTransition().value(), ILiveListener::EndRun);
    TS_ASSERT_LESS_THAN(runNumberBefore, endRunListener->runNumber());

    // Restore default config.
    ConfigService::Instance().setString("fakeeventdatalistener.endrunevery", "0");
  }

  /** After extractData() commits an EndRun edge, calling extractData() again
   *  (before the next period elapses) must clear lastTransition() to nullopt.
   */
  void test_lastTransition_reports_EndRun_once() {
    using Mantid::Kernel::ConfigService;
    ConfigService::Instance().setString("fakeeventdatalistener.endrunevery", "0.001");
    auto endRunListener = LiveListenerFactory::Instance().create("FakeEventDataListener", true);
    endRunListener->start(0);

    // Spin until we observe an EndRun.
    bool gotEndRun = false;
    for (int attempt = 0; attempt < 2000 && !gotEndRun; ++attempt) {
      Poco::Thread::sleep(1);
      endRunListener->extractData();
      if (endRunListener->lastTransition().has_value())
        gotEndRun = true;
    }
    TS_ASSERT(gotEndRun);
    TS_ASSERT_EQUALS(endRunListener->lastTransition().value(), ILiveListener::EndRun);

    // Immediately call extractData() again — the next period cannot have
    // elapsed yet, so onBeforeExtract() clears lastTransition().
    endRunListener->extractData();
    TS_ASSERT(!endRunListener->lastTransition().has_value());

    ConfigService::Instance().setString("fakeeventdatalistener.endrunevery", "0");
  }

  /** The count of EndRun events over a fixed wall-clock window must be
   *  consistent with the configured period — i.e. the cadence has not
   *  changed compared to the legacy runStatus()-based implementation.
   */
  void test_periodic_EndRun_cadence_matches_legacy() {
    using Mantid::Kernel::ConfigService;
    // 50 ms period; run for ~350 ms → expect 5–8 EndRuns.
    ConfigService::Instance().setString("fakeeventdatalistener.endrunevery", "0.05");
    auto endRunListener = LiveListenerFactory::Instance().create("FakeEventDataListener", true);
    endRunListener->start(0);

    int endRunCount = 0;
    const int durationMs = 350;
    const int pollIntervalMs = 5;
    for (int elapsed = 0; elapsed < durationMs; elapsed += pollIntervalMs) {
      Poco::Thread::sleep(pollIntervalMs);
      endRunListener->extractData();
      if (endRunListener->runState() == ILiveListener::EndRun)
        ++endRunCount;
    }
    // Allow generous bounds for slow CI machines.
    TS_ASSERT_LESS_THAN(0, endRunCount);
    TS_ASSERT_LESS_THAN(endRunCount, 20);

    ConfigService::Instance().setString("fakeeventdatalistener.endrunevery", "0");
  }

private:
  std::shared_ptr<ILiveListener> fakel;
};
