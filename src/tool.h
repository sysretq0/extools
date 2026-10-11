// SPDX-License-Identifier: GPL-3.0-only
// The CLI layer: the --ext- namespace, and the two drop-in tool runners.
#ifndef ETOOLS_TOOL_H
#define ETOOLS_TOOL_H

#include <string>
#include <vector>

#include "extflags.h"

namespace etools {

// Runs the dump tool. argv[0] is the invocation name, exactly as dumpsys would
// see it; the rest is the command line. This is dumpsye's whole job.
int run_dumpsys_tool(int argc, char** argv, const ExtFlags& ext);

// Runs the shell-command tool. argv[0] is stripped, matching cmd's own habit of
// indexing argv[0] as the first argument. This is cmde's whole job.
int run_cmd_tool(int argc, char** argv, const ExtFlags& ext);

// The tools' own help text, plus the extensions layered on top of it.
void dumpsyse_usage();
void cmde_usage();

}  // namespace etools

#endif  // ETOOLS_TOOL_H
