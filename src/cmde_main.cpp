// SPDX-License-Identifier: GPL-3.0-only
// cmde - cmd, extended. Like dumpsyse: a front end, with the --ext- namespace
// removed before anything positional is read.
//
// cmd indexes argv[0] as the FIRST ARGUMENT, so the program name is dropped
// here and never reaches the parser - the same thing AOSP's own main does.
#include "extflags.h"
#include "tool.h"

#include <string>
#include <vector>

int main(int argc, char** argv) {
    etools::ExtFlags ext;
    std::vector<std::string> rest;
    if (!etools::parse_ext_flags(argc, argv, etools::ExtTool::kCmd, &ext, &rest)) {
        return 1;
    }
    if (ext.want_help) {
        etools::cmde_usage();
        return 0;
    }

    std::vector<char*> av;
    av.reserve(rest.size());
    for (std::string& s : rest) av.push_back(const_cast<char*>(s.c_str()));
    // No service named is not handled here: the parser below prints the message
    // THIS release uses and returns 20, which is what the real tool does. Its
    // wording changed in API 30 and is not ours to invent.
    // Empty argv (execve with NULL argv): av is empty, so pass 0/nullptr just like
    // dumpsyse; parse_cmd then hits idx >= argc and returns 20 on its own.
    return etools::run_cmd_tool(av.empty() ? 0 : static_cast<int>(av.size()) - 1, av.empty() ? nullptr : av.data() + 1, ext);
}
