// Loading and checking a Turnip (or user-supplied Adreno) driver through libadrenotools: shared by the game
// (vulkan_driver.cpp) and the probe program that tries a driver in a fresh process first (turnip_probe.cpp).
// Nothing here logs: the probe reports through its output.
#pragma once

#include <dlfcn.h>

#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <string>

namespace native {

// Creates a throwaway instance to see which GPU the driver drives; returns the device name ("" if none) and puts a
// line for the log in `report`.
inline std::string ProbeDevice(PFN_vkGetInstanceProcAddr gipa, const char* driver, std::string& report,
                        uint32_t* api_version = nullptr) {
    char line[256];
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    if (!create) {
        snprintf(line, sizeof(line), "vulkan: %s has no vkCreateInstance", driver);
        report = line;
        return "";
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (VkResult r = create(&ci, nullptr, &instance); r != VK_SUCCESS) {
        snprintf(line, sizeof(line), "vulkan: %s vkCreateInstance -> %d", driver, r);
        report = line;
        return "";
    }
    auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    std::string name;
    uint32_t count = 1;
    VkPhysicalDevice device = VK_NULL_HANDLE;
    VkResult r = enumerate ? enumerate(instance, &count, &device) : VK_ERROR_INITIALIZATION_FAILED;
    if (props && r >= 0 && count && device) {
        VkPhysicalDeviceProperties p;
        props(device, &p);
        name = p.deviceName;
        if (api_version) *api_version = p.apiVersion;
        snprintf(line, sizeof(line), "vulkan: %s sees %s (Vulkan %u.%u.%u, driver %08x, vendor %04x device %08x)",
                 driver, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                 VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion, p.vendorID, p.deviceID);
    } else {
        snprintf(line, sizeof(line), "vulkan: %s vkEnumeratePhysicalDevices -> %d, %u device(s)", driver, r, count);
    }
    report = line;
    if (destroy) destroy(instance, nullptr);
    return name;
}

// A driver to try: adrenotools' hook libraries live in the APK's library folder (hook_dir), the driver in dir.
struct Driver {
    std::string hook_dir, dir, file;
    bool custom;  // the user's own driver: any device name is accepted (it may be a Qualcomm build)
};

inline PFN_vkGetInstanceProcAddr LoadTurnip(const Driver& d, std::string& device, std::string& report) {
    // adrenotools joins the directory and the file name as is, so the directories need the trailing slash.
    void* lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, d.hook_dir.c_str(),
                                           d.dir.c_str(), d.file.c_str(), nullptr, nullptr);
    if (!lib) {
        const char* err = dlerror();
        report = std::string("vulkan: adrenotools could not load Turnip (") + (err ? err : "no dlerror") + ")";
        return nullptr;
    }
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) {
        report = "vulkan: the adrenotools loader has no vkGetInstanceProcAddr";
        return nullptr;
    }
    uint32_t api = 0;
    device = ProbeDevice(gipa, d.file.c_str(), report, &api);
    // DXVK needs Vulkan 1.3; Turnip 26.2 offers only 1.0 on an Adreno 610, where 26.3 runs the game.
    if (!device.empty() && api < VK_API_VERSION_1_3) {
        report += " (needs Vulkan 1.3)";
        device.clear();
        return nullptr;
    }
    if (device.empty() || (!d.custom && device.find("Turnip") == std::string::npos)) {
        report += device.empty() ? "" : " (not Turnip: the driver did not load)";
        device.clear();
        return nullptr;
    }
    return gipa;
}

}  // namespace native
