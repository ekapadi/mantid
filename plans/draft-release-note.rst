.. Draft release notes for the live-listener refactoring (sub-specs 01–10).
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

- ``API::LiveListener`` now provides a three-phase template method for
  ``extractData()``.  Subclasses that previously overrode ``extractData()``
  directly should migrate to the three protected hooks:

  - ``onBeforeExtract()`` — runs unconditionally before workspace
    construction; use for pre-extract bookkeeping (e.g. dequeuing a
    pending run-state transition).
  - ``doExtractData()`` — pure virtual; the actual workspace-construction
    step (replaces the previous ``extractData()`` override body).
  - ``onAfterExtract()`` — runs only when ``doExtractData()`` returns
    normally, i.e. on the success path; use for post-extract bookkeeping
    that must **not** execute if extraction throws (e.g.
    ``Exception::NotYet``).  The default implementation is a no-op.

  ``extractData()`` is now ``final`` on ``API::LiveListener``; downstream
  listeners that derive from ``ILiveListener`` directly are unaffected.
