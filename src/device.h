// SPDX-License-Identifier: GPL-3.0-only
// Runtime API-level lookup.
//
// Exactly one thing needs this: AOSP's dumpsys --help text, the set of options
// dumpsys accepts, and cmd's no-arguments message all CHANGE between releases.
// etools has to match the device it is running on, so those three are looked up
// from tables generated out of the AOSP sources (see scripts/gen_compat_text.py).
//
// Nothing ELSE is version-dependent here, but the headers are chosen by a rule
// that inverts the usual "pin the ABI floor" advice: COMPILE UP. The private
// AOSP headers are the NEWEST supported release's, not the oldest. The binary
// links the DEVICE's libbinder at runtime, and on the Itanium C++ ABI a callee
// that ignores an extra register argument is harmless, while a caller that never
// sets one is not:
//
//   * compiling DOWN is broken - IServiceManager::listServices is flagless on
//     API 26/27 and takes an int from API 28. Built against the 26 headers the
//     call passes no flags register, every newer libbinder reads it anyway, and
//     the answer is 0 services where the real tool lists 271 - and nothing
//     errors anywhere. The only symptom is a short list that looks plausible.
//   * compiling UP is safe - an older callee ignores the extra argument.
//
// That is why scripts/fetch_headers.sh defaults to android10-release and treats
// the 8.x point tags as read-only material. NOTES.md section 14 has the detail.
//
// Notably the wire format is not a build-time concern: Parcel::writeInterfaceToken
// is DEFINED in the device's libbinder, so it self-corrects per ROM.

#pragma once

namespace etools {

// ro.build.version.sdk for this process, read once and cached.
// Returns 0 when the property is missing.
//
// 0 means "unknown", not "API 0", and every caller resolves it the same way:
// lookups take the OLDEST variant, because an unknown device is more likely to
// be an old one, and because the failure modes are not symmetric. Serving a new
// release's --help to an old ROM advertises flags the ROM rejects, and serving
// new release's accepted-option set makes etools accept them; serving the oldest
// set can only reject something that would have worked. See variant_for() in
// args.cpp, which is where that rule is implemented.
int device_sdk();

}  // namespace etools