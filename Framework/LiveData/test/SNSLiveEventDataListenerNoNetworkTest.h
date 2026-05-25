// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright © 2018 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "MantidAPI/LiveListenerFactory.h"
#include "MantidLiveData/SNSLiveEventDataListener.h"
#include <cxxtest/TestSuite.h>
#include <stdexcept>

using namespace Mantid::API;
using namespace Mantid::LiveData;

// ---------------------------------------------------------------------------
// Test subclass — exercises protected hooks without a live SMS connection.
// ---------------------------------------------------------------------------
class TestableSNSListener : public SNSLiveEventDataListener {
public:
  int beginRunCount{0};
  int endRunCount{0};
  int pauseCount{0};
  bool lastPauseArg{false};

  // Make protected hooks callable from tests
  void callOnBeginRun() { onBeginRun(); }
  void callOnRunPause(bool p) { onRunPause(p); }

protected:
  void onBeginRun() override {
    ++beginRunCount;
    SNSLiveEventDataListener::onBeginRun();
  }
  void onEndRun() override {
    ++endRunCount;
    SNSLiveEventDataListener::onEndRun();
  }
  void onRunPause(bool p) override {
    ++pauseCount;
    lastPauseArg = p;
    SNSLiveEventDataListener::onRunPause(p);
  }
};

// ---------------------------------------------------------------------------

/**
 * Unit tests for SNSLiveEventDataListener that do not require a live SMS
 * network connection.
 *
 * Tests that require a running SMS server live in SNSLiveEventDataListenerTest.h,
 * which is excluded from the ctest suite until it can be refactored to mock
 * the connection.
 */
class SNSLiveEventDataListenerNoNetworkTest : public CxxTest::TestSuite {
public:
  static SNSLiveEventDataListenerNoNetworkTest *createSuite() { return new SNSLiveEventDataListenerNoNetworkTest(); }
  static void destroySuite(SNSLiveEventDataListenerNoNetworkTest *suite) { delete suite; }

  /** Verify that the m_isDasPaused rename did not invert the pause flag.
   *  A freshly created listener (without connecting) must report "not paused".
   */
  void test_field_rename_does_not_break_pause_handling() {
    Mantid::Kernel::LiveListenerInfo info("SNSLiveEventDataListener");
    auto listener = LiveListenerFactory::Instance().create(info, /*connect=*/false);
    TS_ASSERT(listener);
    TS_ASSERT(!listener->isPaused());
  }

  /** onBeginRun() must throw when m_deferredRunDetailsPkt is null.
   *  This is the invariant that onBeginRun() should only be called after
   *  rxPacket(NEW_RUN) has stashed the RunStatusPkt.
   */
  void test_onBeginRun_throws_when_deferred_run_details_missing() {
    TestableSNSListener listener;
    // A freshly-constructed listener has no deferred pkt → must throw.
    TS_ASSERT_THROWS(listener.callOnBeginRun(), const std::runtime_error &);
    // The override counted the call before delegating to super.
    TS_ASSERT_EQUALS(1, listener.beginRunCount);
  }

  /** onRunPause() must be dispatched for PAUSE and RESUME without
   *  affecting any other observable state (the test checks it is callable
   *  and toggles correctly).
   */
  void test_onRunPause_is_callable_and_toggles() {
    TestableSNSListener listener;
    TS_ASSERT_THROWS_NOTHING(listener.callOnRunPause(true));
    TS_ASSERT_EQUALS(1, listener.pauseCount);
    TS_ASSERT_EQUALS(true, listener.lastPauseArg);

    TS_ASSERT_THROWS_NOTHING(listener.callOnRunPause(false));
    TS_ASSERT_EQUALS(2, listener.pauseCount);
    TS_ASSERT_EQUALS(false, listener.lastPauseArg);
  }
};
