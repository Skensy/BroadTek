/* SPDX-License-Identifier: MIT */
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>

#include "kern_broadtek.hpp"

static BRTK broadtek;

static const char *bootargOff[]   = { "-broadtekoff" };
static const char *bootargDebug[] = { "-broadtekdbg" };
static const char *bootargBeta[]  = { "-broadtekbeta" };

static void pluginStart() {
    broadtek.init();
}

PluginConfiguration ADDPR(config) = {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Ventura,
    KernelVersion::Tahoe,
    pluginStart
};
