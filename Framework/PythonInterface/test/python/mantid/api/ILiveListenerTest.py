# Mantid Repository : https://github.com/mantidproject/mantid
#
# Copyright &copy; 2025 ISIS Rutherford Appleton Laboratory UKRI,
#   NScD Oak Ridge National Laboratory, European Spallation Source,
#   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
# SPDX - License - Identifier: GPL - 3.0 +
import os
import unittest

from mantid.api import AlgorithmManager, FrameworkManager, ILiveListener, ListenerState, RunStatus
from mantid.kernel import ConfigService


class ILiveListenerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FrameworkManager.Instance()
        ConfigService.updateFacilities(os.path.join(ConfigService.getInstrumentDirectory(), "unit_testing/UnitTestFacilities.xml"))
        ConfigService.setFacility("TEST")

    # ------------------------------------------------------------------
    # Enum registration
    # ------------------------------------------------------------------

    def test_RunStatus_enum_values_are_accessible(self):
        for attr in ("NoRun", "JoiningRun", "BeginRun", "Running", "EndRun"):
            self.assertIsNotNone(getattr(RunStatus, attr), f"RunStatus.{attr} missing")

    def test_ListenerState_enum_values_are_accessible(self):
        for attr in ("Disconnected", "Connected", "ReadWait", "Error"):
            self.assertIsNotNone(getattr(ListenerState, attr), f"ListenerState.{attr} missing")

    # ------------------------------------------------------------------
    # getInstance
    # ------------------------------------------------------------------

    def test_getInstance_returns_None_for_non_live_algorithm(self):
        alg = AlgorithmManager.createUnmanaged("Plus")
        self.assertIsNone(ILiveListener.getInstance(alg))

    def test_getInstance_returns_proxy_for_LoadLiveData(self):
        alg = AlgorithmManager.createUnmanaged("LoadLiveData")
        proxy = ILiveListener.getInstance(alg)
        self.assertIsNotNone(proxy)

    def test_getInstance_returns_proxy_for_StartLiveData(self):
        alg = AlgorithmManager.createUnmanaged("StartLiveData")
        proxy = ILiveListener.getInstance(alg)
        self.assertIsNotNone(proxy)

    # ------------------------------------------------------------------
    # State accessors return None before listener is bound (pre-execute)
    # ------------------------------------------------------------------

    def test_state_accessors_return_None_before_execute(self):
        alg = AlgorithmManager.createUnmanaged("LoadLiveData")
        proxy = ILiveListener.getInstance(alg)
        self.assertIsNotNone(proxy)
        # getLiveListener(false, false) → nullptr → all accessors return None
        self.assertIsNone(proxy.runState)
        self.assertIsNone(proxy.listenerState)
        self.assertIsNone(proxy.lastTransition)
        self.assertIsNone(proxy.isPaused)
        self.assertIsNone(proxy.getLogValue("scan_index"))

    # ------------------------------------------------------------------
    # State accessors after execute with FakeEventDataListener
    # FakeEventDataListener always returns Running / Connected / no lastTransition.
    # ------------------------------------------------------------------

    def test_state_after_execute_with_FakeEventDataListener(self):
        alg = AlgorithmManager.createUnmanaged("LoadLiveData")
        alg.initialize()
        alg.setProperty("Instrument", "FakeEventDataListener")
        alg.setProperty("OutputWorkspace", "ILiveListenerTest_fake_ws")
        alg.execute()

        proxy = ILiveListener.getInstance(alg)
        self.assertIsNotNone(proxy)

        self.assertEqual(RunStatus.Running, proxy.runState)
        self.assertEqual(ListenerState.Connected, proxy.listenerState)
        # FakeEventDataListener returns nullopt from lastTransition()
        self.assertIsNone(proxy.lastTransition)
        # FakeEventDataListener uses the default isPaused() → False
        self.assertFalse(proxy.isPaused)
        # FakeEventDataListener uses the default getLogValue() stub → None
        self.assertIsNone(proxy.getLogValue("any_log"))


if __name__ == "__main__":
    unittest.main()
