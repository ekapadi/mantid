// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright &copy; 2016 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX - License - Identifier: GPL - 3.0 +
#pragma once

#include "MantidAPI/ILiveListener.h"

namespace Mantid {
namespace API {
/**
  Base implementation for common behaviour of all live listener classes. It
  implements the ILiveListener interface.
*/
class MANTID_API_DLL LiveListener : public API::ILiveListener {
public:
  bool dataReset() override;
  void setSpectra(const std::vector<specnum_t> &specList) override;
  void setAlgorithm(const class IAlgorithm &callingAlgorithm) override;

  /** Template-method entry point for extracting accumulated data.
   *
   *  Sealed (`final`) on `API::LiveListener` so that all subclasses share a
   *  common contract: any pre-extraction work belongs in `onBeforeExtract()`,
   *  the actual workspace construction belongs in `doExtractData()`, and any
   *  post-extraction work that must run only after a successful hand-off
   *  belongs in `onAfterExtract()`.
   *
   *  Behaviour:
   *    1. `onBeforeExtract()` is invoked on the foreground thread.
   *    2. `doExtractData()` is invoked.
   *    3. `onAfterExtract()` is invoked only if step 2 returned normally.
   *    4. The workspace from step 2 is returned.
   *
   *  Exception safety:
   *    - If `onBeforeExtract()` throws, `doExtractData()` is NOT called and
   *      the exception propagates to the caller (`LoadLiveData`).
   *    - If `doExtractData()` throws (e.g. `Exception::NotYet`) the side
   *      effects performed by `onBeforeExtract()` are preserved — required
   *      by the v3 §5.3 retry-loop contract.
   *    - If `onAfterExtract()` throws, the exception propagates and the
   *      workspace returned by `doExtractData()` is discarded during unwind.
   *
   *  Listeners that derive directly from `ILiveListener` (not from
   *  `LiveListener`) must continue to override `extractData()` themselves.
   */
  std::shared_ptr<Workspace> extractData() final;

protected:
  /** Hook invoked by `extractData()` immediately before `doExtractData()`.
   *  Default is a no-op. Subclasses use it to commit queued FSM transitions
   *  (SNS), advance a synthetic clock (Fake), poll a remote endpoint (SINQ),
   *  etc. May throw to abort the extraction.
   */
  virtual void onBeforeExtract();

  /** Listener-specific workspace-construction step invoked by the
   *  `extractData()` template method. Replaces what subclasses previously
   *  overrode as `extractData()`.
   */
  virtual std::shared_ptr<Workspace> doExtractData() = 0;

  /** Hook invoked by `extractData()` immediately after `doExtractData()`
   *  returns successfully. Default is a no-op. This hook is not called when
   *  `doExtractData()` throws.
   */
  virtual void onAfterExtract();

  /// Indicates receipt of a reset signal from the DAS.
  bool m_dataReset = false;
};

} // namespace API
} // namespace Mantid
