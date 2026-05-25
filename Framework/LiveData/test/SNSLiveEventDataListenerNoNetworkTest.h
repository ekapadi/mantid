// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright © 2018 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "MantidAPI/LiveListenerFactory.h"
#include <cxxtest/TestSuite.h>

using namespace Mantid::API;

/**
 * Unit tests for SNSLiveEventDataListener that do not require a live SMS
 * network connection. Uses the factory with connect=false so that only
 * construction-time state is exercised.
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
   *  A freshly created listener (without connecting) must report "not paused"
   *  so that event-gating in rxPacket(BankedEventPkt) starts in the open state.
   */
  void test_field_rename_does_not_break_pause_handling() {
    Mantid::Kernel::LiveListenerInfo info("SNSLiveEventDataListener");
    auto listener = LiveListenerFactory::Instance().create(info, /*connect=*/false);
    TS_ASSERT(listener);
    TS_ASSERT(!listener->isPaused());
  }
};
