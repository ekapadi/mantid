// Mantid Repository : https://github.com/mantidproject/mantid
//
// Copyright &copy; 2013 ISIS Rutherford Appleton Laboratory UKRI,
//   NScD Oak Ridge National Laboratory, European Spallation Source,
//   Institut Laue - Langevin & CSNS, Institute of High Energy Physics, CAS
// SPDX - License - Identifier: GPL - 3.0 +
#pragma once

//----------------------------------------------------------------------
// Includes
//----------------------------------------------------------------------
#include <stdexcept>

namespace Mantid {
namespace LiveData {
namespace Exception {

/** An exception that can be thrown by an ILiveListener implementation to
    notify LoadLiveData that it is not yet ready to return data. This could
    be, for example, because it has not yet completed its initialisation step
    or if the instrument from which data is being read is not in a run.
    LoadLiveData will ask for data again after a short delay.  Other exceptions
    thrown by the listener will have the effect of stopping the algorithm.
*/
class NotYet : public std::runtime_error {
public:
  /** Constructor.
   *  @param message A description of the exceptional condition.
   *
   *  The constructor prepends a fixed @c "NotYet: " prefix to the
   *  caller-supplied message.  This is a deliberate cross-dylib
   *  workaround, not a cosmetic change.
   *
   *  This class is header-only, has no @c MANTID_LIVEDATA_DLL export
   *  macro, and no key function (its only virtual member, the destructor,
   *  is inherited from @c std::runtime_error without override).  Its
   *  typeinfo is therefore emitted as a weak symbol in every translation
   *  unit that names it.  Under the @c -fvisibility=hidden default used
   *  on macOS, the typeinfo emitted into @c libMantidLiveData.dylib and
   *  the typeinfo emitted into a client executable (e.g. a unit-test
   *  binary) are distinct @c std::type_info objects at runtime.  The
   *  Itanium C++ ABI rule used by libc++abi for @c catch matching is
   *  typeinfo pointer equality, so a @c catch(Exception::NotYet&) in the
   *  client fails to intercept a @c NotYet thrown from the dylib — the
   *  exception escapes to @c std::terminate even though libc++abi
   *  correctly prints the demangled name in its uncaught-exception
   *  diagnostic.  (Linux/libstdc++ does a name-based fallback when
   *  pointer equality fails, which is why the same code catches on Linux.)
   *
   *  Until this class is given a proper @c MANTID_LIVEDATA_DLL export (or
   *  a non-inline key function), callers that need to discriminate
   *  @c NotYet from other exceptions across the dylib boundary must catch
   *  by a base whose typeinfo lives in libc++/libstdc++ (e.g.
   *  @c std::exception) and inspect @c what() for the @c "NotYet: "
   *  prefix.
   */
  explicit NotYet(const std::string &message) : std::runtime_error("NotYet: " + message) {}
};

} // namespace Exception
} // namespace LiveData
} // namespace Mantid
