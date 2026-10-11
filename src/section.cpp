// SPDX-License-Identifier: GPL-3.0-only
#include <etools/section.hpp>

#include <cctype>

namespace etools {
namespace {

// Both grammars are matched at column 0 and must be short: AOSP's own regexes
// cap the title at 70 characters, and long ones are data, not headers.
constexpr size_t kMaxHeaderLen = 72;

std::string_view trim_right(std::string_view s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Case-insensitive "does `needle` occur in `hay`", as a substring. Used by the
// word-boundary matcher below, which needs the position.
size_t find_ci(std::string_view hay, std::string_view needle, size_t from = 0) {
    if (needle.empty() || needle.size() > hay.size()) return std::string_view::npos;
    for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() &&
               std::tolower(static_cast<unsigned char>(hay[i + j])) ==
                   std::tolower(static_cast<unsigned char>(needle[j]))) {
            ++j;
        }
        if (j == needle.size()) return i;
    }
    return std::string_view::npos;
}

// True when `want` occurs in `name` starting at a word boundary: either at the
// beginning, or after something that is not a letter or digit. So "Brightness"
// matches `BrightnessTracker state` and `Automatic Brightness Controller` but not
// `AutomaticBrightnessStrategy`.
bool matches_on_word_boundary(const std::string& name, const std::string& want) {
    size_t at = find_ci(name, want);
    while (at != std::string_view::npos) {
        if (at == 0) return true;
        const unsigned char before = static_cast<unsigned char>(name[at - 1]);
        if (!std::isalnum(before)) return true;
        at = find_ci(name, want, at + 1);
    }
    return false;
}

// A query is normalised the same way a header name is, so `--section` accepts what
// a reader would copy off the header line: trailing whitespace, and a trailing ':'
// that is not part of the name. Without this, `--section "Battery saving stats:"`
// - the exact text of the header - matched nothing, which is the least forgiving
// possible behaviour for a lookup tool.
std::string normalise_query(const std::string& want) {
    std::string_view v = trim_right(want);
    if (!v.empty() && v.back() == ':') v.remove_suffix(1);
    return std::string(trim_right(v));
}

// " (dumpsys " marks the START of a banner's parenthetical, and in a service dump
// it also marks the END of the descriptive part of the name: the text before it
// is the section's real title, the text inside the parens is a dumpsys topic name.
//
// `WINDOW MANAGER POLICY STATE (dumpsys window policy)` is titled
// "WINDOW MANAGER POLICY STATE", not "... (dumpsys window policy)". Comparing
// against the whole line is what made `--section POLICY STATE` match nothing.
//
// It is also why the service-level banner is identified positionally: the FIRST
// banner in a dump is the service's own name, and every later one is a section.
constexpr std::string_view kBannerMarker = " (dumpsys ";

std::string_view name_part(std::string_view header) {
    const size_t at = header.find(kBannerMarker);
    if (at == std::string_view::npos) return header;
    std::string_view v = header.substr(0, at);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
}

// Grammar 1: `Title:` — starts with a letter, ends with a single colon, and has
// no other colon in between, so `foo: bar:` does not qualify.
bool is_colon_header(std::string_view line) {
    if (line.size() < 2 || line.size() > kMaxHeaderLen) return false;
    const unsigned char first = static_cast<unsigned char>(line.front());
    if (!std::isalpha(first)) return false;
    // The colon must TERMINATE the line. This is what rejects
    // `Battery Saver is currently: OFF` and `mFoo=bar: baz` - both are data that
    // happens to contain a colon, and treating them as headers would split the
    // section they sit in.
    //
    // (An earlier "and no other colon before it" clause was deleted: reaching it
    // already implies find(':')==rfind(':'), so it rejected nothing.)
    if (line.find(':') != line.size() - 1) return false;
    // No '=' in a title. `mFoo=bar:` and `mFoo=bar (baz):` are key=value data lines
    // that happen to end in a colon, and at column 0 they would each truncate a
    // section to one line. Real dumps contain both.
    return line.find('=') == std::string_view::npos;
}

// `dumpsys input` shows the grammar's limits. Its headers are `Input properties:`,
// `Input Manager State:`, `Event Hub State:` and so on - none contains the words
// "Input Reader". My test guessed that name from `dumpsys meminfo`, where a
// "Input Reader" section really does exist. The matcher was right and the test was
// wrong, which is the correct outcome for a lookup tool: it says so rather than
// inventing an answer.
//
// Note the contrast with `display`, where `Brightness` IS ambiguous: five headers
// contain that word on a word boundary. `Input` is worse still - `Input
// properties`, `Input Manager State`, `Input Processor State` and `Input Dispatcher
// State` all start with it. Ambiguity is reported, not resolved.

// Grammar 2: `NAME (dumpsys ...)`. `window`, `display` and `input` use this.
// Requiring the closing paren matters: `... policy` appears mid-line in some dumps,
// and a looser test would end sections early.
//
// A line may carry BOTH grammars' punctuation. `WINDOW MANAGER CONSTANTS (dumpsys
// window constants):` is a banner with a trailing colon, and `Input Reader State
// (Nums of device: 7):` is a titled header whose parenthetical is not the dumpsys
// marker at all - a real `input` section that must stay visible. So: accept an
// optional trailing colon, require the "(dumpsys " marker to be present, and allow
// any characters before the closing paren.
bool is_banner_header(std::string_view line) {
    std::string_view v = trim_right(line);
    if (!v.empty() && v.back() == ':') {
        v.remove_suffix(1);
        v = trim_right(v);
    }
    if (v.size() < 12 || v.size() > kMaxHeaderLen * 2) return false;
    const unsigned char first = static_cast<unsigned char>(v.front());
    if (!std::isupper(first)) return false;
    if (v.back() != ')') return false;
    constexpr std::string_view kMarker = " (dumpsys ";
    return v.find(kMarker) != std::string_view::npos;
}

// `Input Reader State (Nums of device: 7):` is a section title that happens to
// carry a colon-bearing parenthetical, so grammar 1's "exactly one colon" rejects
// it, and grammar 2 requires the "(dumpsys " marker this line does not have.
// Without this grammar a real `input` section is invisible to --section.
//
// The title before the paren must be words only: rejecting '=' keeps
// `mFoo=bar (baz):` out, since a key with a parenthetical value is data. Spaces
// ARE allowed - the title is `Input Reader State`, three words.
bool is_titled_parenthetical(std::string_view line) {
    std::string_view v = trim_right(line);
    if (v.empty() || v.back() != ':') return false;
    v.remove_suffix(1);
    v = trim_right(v);
    if (v.size() < 4 || v.size() > kMaxHeaderLen * 2) return false;
    if (!std::isalpha(static_cast<unsigned char>(v.front()))) return false;
    if (v.back() != ')') return false;
    const size_t open = v.find('(');
    if (open == std::string_view::npos || open < 2) return false;
    for (size_t i = 0; i < open; ++i) {
        // '=' is excluded explicitly. It is the one non-word character that is
        // common in DATA lines: `mFoo=bar (baz):` is a key with a parenthetical
        // value, not a section title, and without this it truncates a section to
        // one line at column 0.
        if (v[i] == '=') return false;
        const bool word = std::isalnum(static_cast<unsigned char>(v[i])) != 0;
        if (!word && v[i] != ' ' && v[i] != '\t') return false;
    }
    return true;
}

// The header text as a user would name it: trailing whitespace and the trailing
// ':' removed. `Battery saving stats:` -> `Battery saving stats`.
std::string header_text(const std::string& line) {
    std::string_view v = trim_right(line);
    if (!v.empty() && v.back() == ':') v.remove_suffix(1);
    return std::string(trim_right(v));
}

// Splits on '\n' without inventing a trailing empty element. A dump ending in a
// newline must not produce a phantom final line: that made every service's LAST
// section one line longer than the reference extractor during measurement.
std::vector<std::string_view> split_lines(const std::string& dump) {
    std::vector<std::string_view> out;
    size_t start = 0;
    while (start <= dump.size()) {
        const size_t nl = dump.find('\n', start);
        if (nl == std::string::npos) {
            if (start < dump.size()) out.emplace_back(dump.data() + start, dump.size() - start);
            break;
        }
        out.emplace_back(dump.data() + start, nl - start);
        start = nl + 1;
    }
    return out;
}

bool header_at(const std::string_view line) {
    return is_colon_header(trim_right(line)) || is_banner_header(line) ||
           is_titled_parenthetical(line);
}

}  // namespace

bool is_section_header(const std::string& line) {
    return header_at(line);
}

std::vector<std::string> list_section_headers(const std::string& dump) {
    std::vector<std::string> out;
    for (const std::string_view line : split_lines(dump)) {
        if (is_section_header(std::string(line))) out.push_back(header_text(std::string(line)));
    }
    return out;
}

SectionMatch extract_section(const std::string& dump, const std::string& want,
                             std::vector<std::string>* body, std::vector<std::string>* candidates,
                             std::string* matched_name) {
    const std::vector<std::string_view> lines = split_lines(dump);
    const std::string query = normalise_query(want);

    // Pass one: every header whose text matches `want` on a word boundary.
    //
    // The boundary rule is not a nicety. `dumpsys display` contains both
    // `DisplayBrightnessController:` and `AutomaticBrightnessStrategy:`, so a
    // bare substring search for "Brightness" returns TEN candidates and refuses to
    // answer at all. Requiring the match to start the name, or to follow a
    // non-alphanumeric character, makes `--section Brightness` mean
    // `BrightnessTracker state` and nothing else - while still letting
    // `--section saving stats` find `Battery saving stats:`.
    //
    // Worth recording why this looked fine before: the measurement against awk
    // used bare substring matching on both sides, so the two "agreed" by being
    // wrong in exactly the same way. Agreement on a bug is not accuracy.
    std::vector<size_t> hits;
    std::vector<std::string> names;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!header_at(lines[i])) continue;
        // Skip the service's OWN NAME, and only when it is actually there.
        //
        // The banner this skips is the top-level one a service opens with -
        // `INPUT MANAGER (dumpsys input)`, `POWER MANAGER (dumpsys power)`. It is
        // grammar 2, so it is found as a header, and it must not be matched: it
        // names the service, not a section of it. Without this, `Input Reader`
        // matched that banner and returned all 572 lines of the dump.
        //
        // Identified POSITIONALLY, as a banner on the first line. Keying off the
        // text instead is wrong for `window`, which uses this same banner shape
        // for all eleven of its sections - so "has a (dumpsys marker)" cannot mean
        // "is the service name".
        //
        // The earlier rule skipped whichever header came first, which silently ate
        // the first real section of every service that does not open with a banner.
        // Measured on device: 8 of 12 services probed do not - battery
        // ("Current Battery Service state:"), batterystats ("Battery History ..."),
        // meminfo ("Applications Memory Usage ..."), notification, package, audio,
        // wifi and deviceidle. In batterystats that made "Battery History" (the
        // section you most want) unreachable while "Per-PID Stats" worked.
        //
        // The narrow rule - a banner grammar on the FIRST LINE - costs nothing,
        // because a banner is only ever the service's own name when it opens the
        // dump. `window` puts its banner on line 3 behind "Dump time :", and that
        // banner IS a section ("WINDOW MANAGER LAST ANR"), which this restores.
        if (i == 0 && is_banner_header(lines[0])) continue;

        // Compare against the title only. For a banner that drops the
        // `(dumpsys ...)` suffix; for a plain `Title:` header name_part is the
        // identity, so both grammars go through the same expression.
        const std::string title(name_part(header_text(std::string(lines[i]))));
        if (matches_on_word_boundary(title, query)) {
            hits.push_back(i);
            names.push_back(title);
        }
    }

    // Cleared up front on every path, not just on success. A caller that reuses one
// buffer across several lookups would otherwise read a previous section's lines
// back out of an ambiguous or failed one.
    if (body != nullptr) body->clear();
    if (candidates != nullptr) candidates->clear();
    if (matched_name != nullptr) matched_name->clear();

    if (hits.empty()) return SectionMatch::kNotFound;

    if (hits.size() > 1) {
        // Refuse rather than guess. Duplicate header text is common - wifi repeats
        // `Locks held` 33 times - and silently returning the first one is how you
        // get the wrong section with no indication anything went wrong.
        if (candidates != nullptr) *candidates = names;
        return SectionMatch::kAmbiguous;
    }

    const size_t begin = hits.front();
    size_t end = lines.size();
    for (size_t i = begin + 1; i < lines.size(); ++i) {
        if (!header_at(lines[i])) continue;
        // A service banner terminates the section just like any other header, so
        // that the banner does not get swallowed into whatever follows. `window`
        // opens with `Dump time :` then a LAST ANR banner before POLICY STATE, and
        // without this the preamble leaked into the extracted body.
        end = i;
        break;
    }
    if (body != nullptr) {
        body->clear();
        body->reserve(end - begin);
        for (size_t i = begin; i < end; ++i) body->emplace_back(lines[i]);
    }
    if (matched_name != nullptr) *matched_name = names.front();
    return SectionMatch::kFound;
}

}  // namespace etools