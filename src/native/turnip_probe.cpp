// Driver probe (Android, Adreno): a small program the game starts in a fresh process to see whether a Turnip or
// user-supplied driver works on this phone before loading it into the game (vulkan_driver.cpp). It is an
// executable named like a library (libturnipprobe.so) so that the APK installer extracts it next to libmain.so,
// the one place an app may run programs from.
//
// Usage: libturnipprobe.so <hook dir> <driver dir> <driver file> <custom 0|1>
// Prints one report line; exits 0 if the driver drives the GPU with Vulkan 1.3, 3 if not.
#include <cstdio>
#include <cstring>
#include <string>

#include "turnip_load.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <hook dir> <driver dir> <driver file> <custom 0|1>\n", argv[0]);
        return 2;
    }
    native::Driver d{argv[1], argv[2], argv[3], strcmp(argv[4], "1") == 0};
    std::string device, report;
    bool ok = native::LoadTurnip(d, device, report) != nullptr;
    fputs(report.c_str(), stdout);
    fflush(stdout);
    return ok ? 0 : 3;
}
