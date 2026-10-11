// SPDX-License-Identifier: GPL-3.0-only
// dumpsyse - dumpsys, extended. The binary is a front end and nothing else:
// every behaviour lives in the library, and every extension lives in the
// --ext- namespace, which is stripped from argv before the tool's own parser
// sees it.
#include "extflags.h"
#include "tool.h"

#include <cstdio>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    etools::ExtFlags ext;
    std::vector<std::string> rest;
    if (!etools::parse_ext_flags(argc, argv, etools::ExtTool::kDumpsys, &ext, &rest)) {
        return 1;
    }
    // No arguments is not an error: `dumpsys` with no service named dumps every
    // service, and that is what this does too.
    if (ext.want_help) {
        etools::dumpsyse_usage();
        return 0;
    }

    // rest[0] is this binary's name, so the parser below sees exactly what
    // dumpsys would have seen. `dumpsyse --help` therefore prints dumpsys's own
    // help, unchanged, because --help is not ours and is never intercepted.
    std::vector<char*> av;
    av.reserve(rest.size());
    for (std::string& s : rest) av.push_back(const_cast<char*>(s.c_str()));
    return etools::run_dumpsys_tool(static_cast<int>(av.size()), av.data(), ext);
}
