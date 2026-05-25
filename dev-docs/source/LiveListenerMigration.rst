.. _LiveListenerMigration:

Live-Listener API Migration Guide (v3 refactoring)
===================================================

Mantid's live-listener interface was refactored in release **6.x** to
separate state-read from state-transition, eliminate hidden side effects in
``runStatus()``, and allow :ref:`LoadLiveData <algm-LoadLiveData>` to run
successfully as a stand-alone algorithm without requiring
:ref:`MonitorLiveData <algm-MonitorLiveData>`.

New API (use these)
-------------------

All four methods are **pure getters** — calling them never mutates internal
state.  They are declared on ``ILiveListener``.

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - Method
     - Meaning
   * - ``runState() const``
     - Current DAS run state: ``NoRun``, ``BeginRun``, ``Running``, or ``EndRun``.
       Updated by the background thread; readable from the foreground at any time.
   * - ``isPaused() const``
     - ``true`` when the DAS has signalled a run pause (ADARA annotation packet).
       Orthogonal to ``runState()`` — the run state remains ``Running`` while paused.
   * - ``listenerState() const``
     - Connection / health: ``Disconnected``, ``ReadWait``, ``Connected``, or ``Error``.
       Now **pure virtual** — every concrete listener must override it.
   * - ``lastTransition() const``
     - ``std::optional<RunStatus>`` — the edge (``BeginRun`` or ``EndRun``) committed
       by the most recent ``extractData()`` call, or ``std::nullopt`` if none.
       Cleared automatically on the *next* successful ``extractData()`` cycle so that
       ``MonitorLiveData`` does not re-process the same edge.

Deprecated API (migrate away from)
-----------------------------------

``runStatus()``
~~~~~~~~~~~~~~~

The old ``runStatus()`` method carried large hidden side effects:

* Cleared geometry and name-map caches.
* Re-initialised the workspace.
* Consumed the deferred ``RunStatusPkt``.
* Cleared the ``m_pauseNetRead`` gate (causing deadlock in stand-alone ``LoadLiveData``).

It is replaced by the combination of ``lastTransition()`` and ``runState()``.  The
default base-class implementation of ``runStatus()`` is now:

.. code-block:: cpp

   RunStatus ILiveListener::runStatus() {
       if (auto edge = lastTransition()) return *edge;
       return runState();
   }

This shim preserves backward-compatible behaviour for ``MonitorLiveData`` without
requiring callers to change immediately.  The method is marked ``[[deprecated]]``
and **will be removed in a future release**.

Migration recipe for listener authors
--------------------------------------

If you maintain a concrete ``ILiveListener`` subclass, migrate as follows:

1. **Remove your ``runStatus()`` override** (if you had one).  The base-class shim
   is correct for most listeners.

2. **Add ``listenerState() const override``** if you have not already done so.  It
   is now pure virtual:

   .. code-block:: cpp

      API::ListenerState listenerState() const override {
          return m_isConnected ? API::ListenerState::Connected
                               : API::ListenerState::Disconnected;
      }

3. **Optionally override ``runState()``, ``isPaused()``, ``lastTransition()``** to
   expose fine-grained state.  Simple listeners that have no run-boundary concept
   can leave the defaults in place.

4. **Do not override ``extractData()``** — override ``doExtractData()`` instead.
   The base class ``LiveListener::extractData()`` is ``final``; it calls
   ``onBeforeExtract()`` (the commit hook) before delegating to ``doExtractData()``.

State-transition hooks
----------------------

Listeners that need to react to run boundaries should override the *hook* methods
rather than the getters:

.. list-table::
   :header-rows: 1
   :widths: 25 75

   * - Hook
     - When called
   * - ``onBeginRun()``
     - Called by ``extractData()`` when a ``BeginRun`` transition is pending.
       The new run details are available at this point.
   * - ``onEndRun()``
     - Called by ``extractData()`` when an ``EndRun`` transition is pending.
   * - ``onRunPause(bool paused)``
     - Called from the background thread when a DAS pause/resume annotation
       is received.  ``paused = true`` for pause, ``false`` for resume.
   * - ``onBeforeExtract()``
     - Called at the very start of ``extractData()``, before ``doExtractData()``.
       Override here for any per-extraction bookkeeping.

All hooks have no-op default implementations in ``LiveListener`` and may be
selectively overridden.

Further reading
---------------

* ``plans/listener_refactoring_v3.md`` — full design specification.
* ``plans/listener_refactoring_other_listeners.md`` — worked examples for
  ``SINQHMListener``, ``FakeEventDataListener``, and others.
* ``Framework/API/inc/MantidAPI/ILiveListener.h`` — authoritative interface.
* ``Framework/API/inc/MantidAPI/LiveListener.h`` — ``extractData()`` template
  method and default hook implementations.
