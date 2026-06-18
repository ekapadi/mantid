// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright &copy; 2025 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX - License - Identifier: GPL - 3.0 +

// Exports a partial, read-only Python view of ILiveListener that can be
// obtained from any LiveDataAlgorithm instance (StartLiveData, LoadLiveData,
// MonitorLiveData) via ILiveListener.getInstance(alg).
//
// Only the const state queries and getLogValue are exposed; mutating
// operations (connect, start, extractData, etc.) are deliberately omitted.

#include "MantidAPI/ILiveListener.h"
#include "MantidAPI/IAlgorithm.h"
#include "MantidKernel/Property.h"
#include "MantidLiveData/LiveDataAlgorithm.h"

#include <boost/python/class.hpp>
#include <boost/python/enum.hpp>
#include <boost/python/handle.hpp>
#include <boost/python/manage_new_object.hpp>
#include <boost/python/object.hpp>

using namespace boost::python;
using Mantid::API::IAlgorithm_sptr;
using Mantid::API::ILiveListener;
using Mantid::API::ILiveListener_sptr;
using Mantid::API::ListenerState;
using Mantid::Kernel::Property;
using Mantid::LiveData::LiveDataAlgorithm;

namespace {

// Returns the cached listener from alg (observe-only; never creates a new one).
ILiveListener_sptr resolveListener(const IAlgorithm_sptr &alg) {
  auto *lda = dynamic_cast<LiveDataAlgorithm *>(alg.get());
  if (!lda)
    return nullptr;
  return lda->getLiveListener(/*start=*/false, /*createIfMissing=*/false);
}

// --------------------------------------------------------------------------
// Proxy wrapping an IAlgorithm_sptr.  Holds the LiveDataAlgorithm reference
// and re-resolves the listener on every accessor call, so the return value
// always reflects current state even if the listener is bound after
// getInstance() was called.
// --------------------------------------------------------------------------
class LiveListenerProxy {
public:
  explicit LiveListenerProxy(IAlgorithm_sptr alg) : m_alg(std::move(alg)) {}

  // Returns a LiveListenerProxy object or None if alg is not a LiveDataAlgorithm.
  static object getInstance(const IAlgorithm_sptr &alg) {
    if (!dynamic_cast<LiveDataAlgorithm *>(alg.get()))
      return object();
    return object(LiveListenerProxy(alg));
  }

  object runState() const {
    auto l = resolveListener(m_alg);
    if (!l)
      return object();
    return object(l->runState()); // enum_ converter installed by export_ILiveListener
  }

  object listenerState() const {
    auto l = resolveListener(m_alg);
    if (!l)
      return object();
    return object(l->listenerState());
  }

  object lastTransition() const {
    auto l = resolveListener(m_alg);
    if (!l)
      return object();
    const auto opt = l->lastTransition();
    if (!opt)
      return object();
    return object(*opt);
  }

  object isPaused() const {
    auto l = resolveListener(m_alg);
    if (!l)
      return object();
    return object(l->isPaused());
  }

  // Returns a raw owning pointer; the Python wrapper in getLogValuePy
  // transfers ownership to Python via manage_new_object.
  Property *getLogValueRaw(const std::string &name) const {
    auto l = resolveListener(m_alg);
    if (!l)
      return nullptr;
    return l->getLogValue(name).release();
  }

  IAlgorithm_sptr m_alg;
};

// Intercepts nullptr before handing to manage_new_object so that Python
// receives None rather than a null-wrapped object.
object getLogValuePy(const LiveListenerProxy &self, const std::string &name) {
  Property *raw = self.getLogValueRaw(name);
  if (!raw)
    return object();
  using MNO = manage_new_object::apply<Property *>::type;
  return object(handle<>(MNO()(raw)));
}

} // namespace

void export_ILiveListener() {
  // Enums — registered before the class so their to_python converters are
  // available when proxy accessor methods are called from Python.
  enum_<ILiveListener::RunStatus>("RunStatus")
      .value("NoRun", ILiveListener::NoRun)
      .value("JoiningRun", ILiveListener::JoiningRun)
      .value("BeginRun", ILiveListener::BeginRun)
      .value("Running", ILiveListener::Running)
      .value("EndRun", ILiveListener::EndRun)
      .export_values();

  enum_<ListenerState>("ListenerState")
      .value("Disconnected", ListenerState::Disconnected)
      .value("Connected", ListenerState::Connected)
      .value("ReadWait", ListenerState::ReadWait)
      .value("Error", ListenerState::Error)
      .export_values();

  // Partial, read-only ILiveListener view bound to a LiveDataAlgorithm.
  class_<LiveListenerProxy>("ILiveListener", no_init)
      .def("getInstance", &LiveListenerProxy::getInstance, arg("alg"),
           "Obtain a read-only live-listener view from any LiveDataAlgorithm "
           "(StartLiveData, LoadLiveData, MonitorLiveData).\n"
           "Returns None if alg is not a LiveDataAlgorithm.\n"
           "All property accessors return None when no listener is currently bound "
           "(e.g. before execute() or when MonitorLiveData has released its listener).")
      .staticmethod("getInstance")
      .add_property("runState", &LiveListenerProxy::runState,
                    "DAS run state (RunStatus enum), or None when no listener is bound.")
      .add_property("listenerState", &LiveListenerProxy::listenerState,
                    "Listener connection/health (ListenerState enum), or None when no listener is bound.")
      .add_property("lastTransition", &LiveListenerProxy::lastTransition,
                    "Run-state edge committed by the most recent extractData() "
                    "(RunStatus enum), or None if no transition has occurred or no listener is bound.")
      .add_property("isPaused", &LiveListenerProxy::isPaused,
                    "True if the DAS has sent a pause annotation, False otherwise, "
                    "or None when no listener is bound.")
      .def("getLogValue", getLogValuePy, (arg("self"), arg("name")),
           "Return a cloned Property snapshot for the named PV log, or None if "
           "the log is not present or no listener is bound.\n"
           "The returned object is owned by the caller and is stable against "
           "concurrent background-thread updates (it is a copy taken under the "
           "listener's internal lock).");
}
