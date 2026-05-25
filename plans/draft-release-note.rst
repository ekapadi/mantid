.. Draft release notes for the live-listener refactoring (sub-specs 01–09).
.. Copy the relevant items into the real v6.x release directories when
.. the PR is merged.  See dev-docs/source/LiveListenerMigration.rst for the
.. developer migration guide.

Bugfixes
--------

- :ref:`LoadLiveData <algm-LoadLiveData>` no longer deadlocks when used
  stand-alone (i.e. without :ref:`MonitorLiveData <algm-MonitorLiveData>`)
  after a run-state boundary (BeginRun / EndRun packet).

New Features
------------

- ``ILiveListener`` now exposes four pure-getter state queries that carry
  no side effects:

  - ``runState()`` — current DAS run state (``NoRun`` / ``BeginRun`` /
    ``Running`` / ``EndRun``).
  - ``isPaused()`` — whether the DAS has signalled a run pause (orthogonal
    to ``runState()``).
  - ``listenerState()`` — connection / health of the listener
    (``Disconnected`` / ``ReadWait`` / ``Connected`` / ``Error``).
  - ``lastTransition()`` — the run-state edge (if any) committed by the most
    recent ``extractData()`` call, as a ``std::optional<RunStatus>``.

  The existing ``runStatus()`` method is now deprecated; see the migration
  guide in ``dev-docs/source/LiveListenerMigration.rst``.
