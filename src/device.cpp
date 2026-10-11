// SPDX-License-Identifier: GPL-3.0-only
#include "device.h"

#include <sys/system_properties.h>

#include <cstdlib>

namespace etools {
namespace {
int g_cached = -1;   // -1 = not read yet
}

int device_sdk() {
    if (g_cached >= 0) return g_cached;
    // __system_property_get lives in LIBC, so this adds no dependency beyond
    // what libbinder already pulls in.
    char buf[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.build.version.sdk", buf) <= 0) {
        g_cached = 0;   // unknown, not "API 0"
    } else {
        g_cached = atoi(buf);
    }
    return g_cached;
}

}  // namespace etools