// SPDX-License-Identifier: GPL-3.0-only
// Section extraction from a captured dumpsys dump.
//
// Why this exists as its own file: the grammar is the part of etools that is most
// likely to be subtly wrong, and it is pure string work with no binder in it, so
// it can be tested without a device round trip. test_flags covers it directly.
//
// The grammar was NOT invented. It was measured against 12 real service dumps and
// cross-checked against the device's own awk, section by section, on identical
// bytes. Two things that came out of that measurement and would otherwise have
// been gotchas:
//
//   * Column-0 anchoring is not negotiable. Allowing any indentation takes
//     `package` from 41 headers to 5467, because every package entry contains
//     lines like `Disabled:` and `Selected from:`.
//   * Two grammars are needed, because services disagree. `power` writes
//     `Battery saving stats:`; `window` writes `WINDOW MANAGER POLICY STATE
//     (dumpsys window policy)` with no colon at all. Both appear inside the same
//     service (activity has 9 of each).
//
// Deliberately NOT implemented: indentation, nesting paths, and regex. See
// NOTES.md for why each was measured and rejected.

#pragma once

#include <string>
#include <vector>

namespace etools {

// What extract_section() found.
enum class SectionMatch {
    kFound,       // exactly one section matched; `body` holds it
    kAmbiguous,   // several matched; `candidates` lists them, nothing emitted
    kNotFound,    // none matched; caller decides whether to fall back
};

// Splits a captured dump into sections and returns the one matching `want`.
//
// `want` is matched case-insensitively against the header text, and the match
// must START on a word boundary - at the beginning of the name, or after a
// non-alphanumeric character. It does not have to END on one, so "Brightness"
// finds `BrightnessTracker state`. A match must be unique.
//
// Exact-match-first was tried and measured: it is worse. `dumpsys package` has
// both `Known Packages` (line 9) and `Packages` (line 26315), so exact-first
// returned 18,131 lines where a substring match returns the 43-line section a
// caller actually meant. Being more precise about the name made the answer
// worse. Uniqueness is what catches real mistakes instead: 33 headers in
// `dumpsys wifi` repeat (`Locks held` appears twice), where a first-match-wins
// matcher silently returns one of them with no indication which.
//
// Note that the exact-match argument above no longer applies to this ROM under
// the CURRENT matcher: `--section Packages` against API 34's `dumpsys package`
// is AMBIGUOUS, not a 43-line answer, because the word also appears in `Hidden
// system packages`, `Frozen packages` and four `... APEX packages` headers.
// Refusing is the right outcome and is what it does; the historical measurement
// is kept because it is why substring matching is the default.
//
// The left-boundary rule is what stops that same search matching inside longer
// words: "Bright" must not find `BrightnessController`.
//
// The returned body includes the header line itself and ends immediately before
// the next header. On kFound, `matched_name` is the header text that matched.
SectionMatch extract_section(const std::string& dump, const std::string& want,
                             std::vector<std::string>* body, std::vector<std::string>* candidates,
                             std::string* matched_name);

// True if `line` is a section header under either grammar. Exposed for tests.
bool is_section_header(const std::string& line);

// Every header in `dump`, in order, with its line text. Exposed for tests.
std::vector<std::string> list_section_headers(const std::string& dump);

}  // namespace etools