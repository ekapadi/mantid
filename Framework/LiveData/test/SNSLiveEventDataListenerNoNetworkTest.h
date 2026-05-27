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
// Test subclass — accesses protected fields and hooks without a live SMS
// connection.  Fields used here (m_mutex, m_pendingTransition, etc.) are
// protected in SNSLiveEventDataListener so no friend declaration is needed.
// ---------------------------------------------------------------------------
class TestableSNSListener : public SNSLiveEventDataListener {
public:
  int beginRunCount{0};
  int endRunCount{0};
  int pauseCount{0};
  bool lastPauseArg{false};

  /// When true the overridden hooks just increment counters without calling
  /// the base implementation.  Set this before tests that would throw from
  /// onBeginRun() due to a missing m_deferredRunDetailsPkt.
  bool m_stubHooks{false};

  // --- Helpers to inject protected base-class state -------------------------

  void injectPendingTransition(ILiveListener::RunStatus r) {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Mirror the single-slot invariant from rxPacket(RunStatusPkt).
    if (m_pendingTransition.has_value())
      throw std::runtime_error("TestableSNSListener::injectPendingTransition: slot already occupied");
    m_pendingTransition = r;
  }

  void injectBackgroundException(const std::string &msg) {
    m_backgroundException = std::make_shared<std::runtime_error>(msg);
  }

  void callOnBeginRun() { onBeginRun(); }
  void callOnEndRun() { onEndRun(); }
  void callOnRunPause(bool p) { onRunPause(p); }
  void callOnBeforeExtract() { onBeforeExtract(); }
  void callOnAfterExtract() { onAfterExtract(); }

  ILiveListener::RunStatus readAdaraRunStatus() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_adaraRunStatus;
  }

protected:
  void onBeginRun() override {
    ++beginRunCount;
    if (!m_stubHooks)
      SNSLiveEventDataListener::onBeginRun();
  }
  void onEndRun() override {
    ++endRunCount;
    if (!m_stubHooks)
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

  // -------------------------------------------------------------------------
  // Sub-spec 05 regression: field rename did not invert pause flag
  // -------------------------------------------------------------------------

  /** Verify that the m_isDasPaused rename did not invert the pause flag.
   *  A freshly created listener (without connecting) must report "not paused".
   */
  void test_field_rename_does_not_break_pause_handling() {
    Mantid::Kernel::LiveListenerInfo info("SNSLiveEventDataListener");
    auto listener = LiveListenerFactory::Instance().create(info, /*connect=*/false);
    TS_ASSERT(listener);
    TS_ASSERT(!listener->isPaused());
  }

  // -------------------------------------------------------------------------
  // Sub-spec 06 regression: hooks callable and correctly guarded
  // -------------------------------------------------------------------------

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

  // -------------------------------------------------------------------------
  // Sub-spec 07: pure-getter tests
  // -------------------------------------------------------------------------

  /** runState() is a pure getter: calling it 100× must not change any
   *  observable state and must always return the initial value (NoRun).
   */
  void test_runState_pure_getter_does_not_mutate() {
    TestableSNSListener listener;
    for (int i = 0; i < 100; ++i) {
      TS_ASSERT_EQUALS(ILiveListener::NoRun, listener.runState());
    }
    // Counters untouched: no hook fired.
    TS_ASSERT_EQUALS(0, listener.beginRunCount);
    TS_ASSERT_EQUALS(0, listener.endRunCount);
  }

  /** A freshly constructed (not connected) listener should report Disconnected. */
  void test_listenerState_initially_disconnected() {
    TestableSNSListener listener;
    TS_ASSERT_EQUALS(ListenerState::Disconnected, listener.listenerState());
  }

  /** lastTransition() must be nullopt before any extractData() call. */
  void test_lastTransition_initially_null() {
    TestableSNSListener listener;
    TS_ASSERT(!listener.lastTransition().has_value());
  }

  /** isPaused() must be false on a freshly-constructed listener. */
  void test_isPaused_initially_false() {
    TestableSNSListener listener;
    TS_ASSERT(!listener.isPaused());
  }

  /** isPaused() is orthogonal to runState(): onRunPause toggles isPaused() but
   *  must not alter m_adaraRunStatus.
   */
  void test_isPaused_orthogonal_to_runState() {
    TestableSNSListener listener;
    TS_ASSERT_EQUALS(ILiveListener::NoRun, listener.runState());
    TS_ASSERT(!listener.isPaused());

    listener.callOnRunPause(true);
    TS_ASSERT_EQUALS(ILiveListener::NoRun, listener.runState()); // unchanged
    TS_ASSERT(listener.isPaused());

    listener.callOnRunPause(false);
    TS_ASSERT_EQUALS(ILiveListener::NoRun, listener.runState()); // still unchanged
    TS_ASSERT(!listener.isPaused());
  }

  // -------------------------------------------------------------------------
  // Sub-spec 07: onBeforeExtract / onAfterExtract dispatch tests
  // -------------------------------------------------------------------------

  /** onBeforeExtract() with a queued BeginRun must dispatch exactly once and
   *  set lastTransition() == BeginRun.
   */
  void test_onBeforeExtract_dispatches_BeginRun_to_hook() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;
    listener.injectPendingTransition(ILiveListener::BeginRun);

    TS_ASSERT_THROWS_NOTHING(listener.callOnBeforeExtract());
    TS_ASSERT_EQUALS(1, listener.beginRunCount);
    TS_ASSERT_EQUALS(0, listener.endRunCount);
    TS_ASSERT(listener.lastTransition().has_value());
    TS_ASSERT_EQUALS(ILiveListener::BeginRun, *listener.lastTransition());
  }

  /** onAfterExtract() with a queued EndRun must dispatch exactly once and
   *  set lastTransition() == EndRun.  onBeforeExtract() must not dispatch
   *  EndRun (EndRun is deferred so doExtractData() can harvest the buffer).
   */
  void test_onAfterExtract_dispatches_EndRun_to_hook() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;
    listener.injectPendingTransition(ILiveListener::EndRun);

    // onBeforeExtract must NOT dispatch EndRun.
    TS_ASSERT_THROWS_NOTHING(listener.callOnBeforeExtract());
    TS_ASSERT_EQUALS(0, listener.beginRunCount);
    TS_ASSERT_EQUALS(0, listener.endRunCount);
    TS_ASSERT(!listener.lastTransition().has_value());

    // onAfterExtract dispatches EndRun and commits lastTransition.
    TS_ASSERT_THROWS_NOTHING(listener.callOnAfterExtract());
    TS_ASSERT_EQUALS(0, listener.beginRunCount);
    TS_ASSERT_EQUALS(1, listener.endRunCount);
    TS_ASSERT(listener.lastTransition().has_value());
    TS_ASSERT_EQUALS(ILiveListener::EndRun, *listener.lastTransition());
  }

  /** onBeforeExtract() with no pending transition must not dispatch any hook
   *  and must leave lastTransition() in its prior state.
   */
  void test_no_transition_no_hook() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;
    // Pre-condition: no pending transition, lastTransition stays null.
    TS_ASSERT_THROWS_NOTHING(listener.callOnBeforeExtract());
    TS_ASSERT_EQUALS(0, listener.beginRunCount);
    TS_ASSERT_EQUALS(0, listener.endRunCount);
    TS_ASSERT(!listener.lastTransition().has_value());
  }

  // -------------------------------------------------------------------------
  // Sub-spec 07: lastTransition lifecycle
  // -------------------------------------------------------------------------

  /** After a BeginRun transition is committed via onBeforeExtract(), a second
   *  onBeforeExtract() with no pending must leave lastTransition() as BeginRun
   *  (C1 fix: edge survives across NotYet retries).
   */
  void test_lastTransition_survives_NotYet_retry() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;

    // Commit BeginRun.
    listener.injectPendingTransition(ILiveListener::BeginRun);
    listener.callOnBeforeExtract();
    TS_ASSERT_EQUALS(ILiveListener::BeginRun, *listener.lastTransition());

    // Simulate NotYet retry: onBeforeExtract() again with no pending and no
    // successful extract — edge must survive (C1 fix).
    listener.callOnBeforeExtract();
    TS_ASSERT(listener.lastTransition().has_value());
    TS_ASSERT_EQUALS(ILiveListener::BeginRun, *listener.lastTransition());
  }

  /** onAfterExtract() must clear the committed transition after a successful
   *  extract so the edge is not re-processed on the next tick.
   */
  void test_lastTransition_cleared_after_successful_extract() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;

    listener.injectPendingTransition(ILiveListener::BeginRun);
    listener.callOnBeforeExtract();
    TS_ASSERT_EQUALS(ILiveListener::BeginRun, *listener.lastTransition());

    listener.callOnAfterExtract();
    TS_ASSERT(!listener.lastTransition().has_value());
  }

  /** Symmetric test for EndRun: onAfterExtract() commits the EndRun edge and
   *  a subsequent onAfterExtract() (no pending) clears it.
   */
  void test_lastTransition_reports_EndRun_then_null_after_success() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;

    listener.injectPendingTransition(ILiveListener::EndRun);

    // onBeforeExtract() does not dispatch EndRun — lastTransition() stays null.
    listener.callOnBeforeExtract();
    TS_ASSERT(!listener.lastTransition().has_value());

    // onAfterExtract() commits EndRun.
    listener.callOnAfterExtract();
    TS_ASSERT(listener.lastTransition().has_value());
    TS_ASSERT_EQUALS(ILiveListener::EndRun, *listener.lastTransition());

    // After next successful extract (no pending): edge cleared.
    listener.callOnAfterExtract();
    TS_ASSERT(!listener.lastTransition().has_value());
  }

  // -------------------------------------------------------------------------
  // Sub-spec 07: legacy runStatus() shim
  // -------------------------------------------------------------------------

  /** The deprecated runStatus() shim must return the edge when lastTransition()
   *  is set, then fall back to runState() once the edge is cleared.
   */
  void test_legacy_runStatus_returns_edge_then_state() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;

    // Inject a BeginRun transition and commit it.
    listener.injectPendingTransition(ILiveListener::BeginRun);
    listener.callOnBeforeExtract();

    // runStatus() should report the edge.
    TS_ASSERT_EQUALS(ILiveListener::BeginRun, listener.runStatus());

    // Simulate success clearing the edge in the post-extract hook.
    listener.callOnAfterExtract();

    // Now runStatus() must fall back to runState().
    // onBeginRun() was stubbed, so m_adaraRunStatus is still NoRun.
    TS_ASSERT_EQUALS(ILiveListener::NoRun, listener.runStatus());
  }

  // -------------------------------------------------------------------------
  // Sub-spec 07: background exception propagation
  // -------------------------------------------------------------------------

  /** When the background thread has thrown, runState(), lastTransition(), and
   *  runStatus() must all rethrow the stored exception.  listenerState() must
   *  return Error without throwing.
   */
  void test_background_exception_propagates_from_all_getters() {
    TestableSNSListener listener;
    listener.injectBackgroundException("synthetic background error");

    TS_ASSERT_THROWS(listener.runState(), const std::runtime_error &);
    TS_ASSERT_THROWS(listener.lastTransition(), const std::runtime_error &);
    TS_ASSERT_THROWS(listener.runStatus(), const std::runtime_error &);

    // listenerState() must NOT throw — it returns Error to allow callers to
    // distinguish "listener broken" from "listener not yet connected".
    TS_ASSERT_THROWS_NOTHING(listener.listenerState());
    TS_ASSERT_EQUALS(ListenerState::Error, listener.listenerState());
  }

  // -------------------------------------------------------------------------
  // Sub-spec 07: single-slot invariant
  // -------------------------------------------------------------------------

  /** Injecting a second transition while one is pending must throw.
   *  This directly tests the invariant enforced by injectPendingTransition()
   *  which mirrors the check in rxPacket(RunStatusPkt).
   */
  void test_pending_transition_queue_invariant_violation_throws() {
    TestableSNSListener listener;
    listener.m_stubHooks = true;
    listener.injectPendingTransition(ILiveListener::BeginRun);

    // A second injection before the first is consumed must throw.
    TS_ASSERT_THROWS(listener.injectPendingTransition(ILiveListener::EndRun), const std::runtime_error &);
  }
};
