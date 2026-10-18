#include "cases.hpp"
#include "lifecycle_policy.hpp"
#include "report.hpp"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_beta.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <crtdbg.h>
#include <cstdlib>
#endif

// NVRHI is linked statically and shares this Vulkan-Hpp dispatcher. The pinned
// loader is also supplied explicitly to GLFW; no second loader is selected.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace {

using shark::probe::Json;
using Object = Json::Object;
using Array = Json::Array;

struct Failure final : std::runtime_error {
    Failure(std::string codeValue, std::string message, int exitValue = 1)
        : std::runtime_error(std::move(message)), code(std::move(codeValue)), exitCode(exitValue) {}
    std::string code;
    int exitCode;
};

struct Options final {
    std::string mode;
    std::string device;
    bool validation = true;
    std::filesystem::path shaderDirectory;
    std::filesystem::path artifactsDirectory;
    std::string injectedFailure = "none";
    std::uint32_t lifecycleCycles = 2;
    std::uint32_t deviceCycles = 1;
    std::uint32_t presentSeconds = 0;
};

Options parseOptions(int argc, char** argv) {
    Options options;
    bool deviceSeen = false;
    bool validationSeen = false;
    std::vector<std::string> caseOptions;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "--version" || argument == "--describe" ||
            argument == "--inventory" || argument == "--smoke" || argument == "--cases") {
            if (!options.mode.empty()) {
                throw Failure("arguments", "Choose exactly one probe mode", 2);
            }
            options.mode = argument.substr(2);
        } else if (argument == "--device") {
            if (deviceSeen || index + 1 == argc) {
                throw Failure("arguments", "--device requires one unique Vulkan deviceUUID", 2);
            }
            deviceSeen = true;
            options.device = argv[++index];
            if (options.device.size() != VK_UUID_SIZE * 2 ||
                !std::all_of(options.device.begin(), options.device.end(), [](const char value) {
                    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
                })) {
                throw Failure(
                    "arguments",
                    "--device must be exactly 32 lowercase hexadecimal digits from --inventory", 2);
            }
        } else if (argument == "--validation") {
            if (validationSeen || index + 1 == argc) {
                throw Failure("arguments", "--validation requires one value: required or off", 2);
            }
            validationSeen = true;
            const std::string_view value(argv[++index]);
            if (value != "required" && value != "off") {
                throw Failure("arguments", "--validation must be required or off", 2);
            }
            options.validation = value == "required";
        } else if (argument == "--shader-dir" || argument == "--artifacts-dir" ||
                   argument == "--inject-failure" || argument == "--lifecycle-cycles" ||
                   argument == "--device-cycles" || argument == "--present-seconds") {
            if (index + 1 == argc ||
                std::find(caseOptions.begin(), caseOptions.end(), argument) != caseOptions.end()) {
                throw Failure("arguments",
                              "Case option requires one unique value: " + std::string(argument), 2);
            }
            caseOptions.emplace_back(argument);
            const std::string value(argv[++index]);
            if (argument == "--shader-dir") {
                options.shaderDirectory = value;
            } else if (argument == "--artifacts-dir") {
                options.artifactsDirectory = value;
            } else if (argument == "--inject-failure") {
                if (value != "none" && value != "missing-feature" && value != "shader" &&
                    value != "readback") {
                    throw Failure("arguments", "Unknown --inject-failure value", 2);
                }
                options.injectedFailure = value;
            } else {
                std::uint32_t number = 0;
                const auto parsed =
                    std::from_chars(value.data(), value.data() + value.size(), number);
                const std::uint32_t maximum = argument == "--present-seconds" ? 600U
                                              : argument == "--device-cycles" ? 20U
                                                                              : 100U;
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                    number > maximum || (argument == "--device-cycles" && number == 0)) {
                    throw Failure("arguments", "Out-of-range case option: " + std::string(argument),
                                  2);
                }
                if (argument == "--lifecycle-cycles")
                    options.lifecycleCycles = number;
                if (argument == "--device-cycles")
                    options.deviceCycles = number;
                if (argument == "--present-seconds")
                    options.presentSeconds = number;
            }
        } else {
            throw Failure("arguments", "Unknown argument: " + std::string(argument), 2);
        }
    }
    if (options.mode.empty()) {
        throw Failure("arguments", "Choose --describe, --inventory, --smoke, --help or --version",
                      2);
    }
    if (options.mode == "smoke" || options.mode == "cases") {
        if (!deviceSeen) {
            throw Failure("arguments",
                          "--smoke/--cases requires --device <deviceUUID>; run --inventory first",
                          2);
        }
    } else if (deviceSeen || validationSeen) {
        throw Failure("arguments", "--device and --validation require --smoke or --cases", 2);
    }
    if (options.mode == "cases") {
        if (options.shaderDirectory.empty() || options.artifactsDirectory.empty()) {
            throw Failure("arguments", "--cases requires --shader-dir and --artifacts-dir", 2);
        }
    } else if (!caseOptions.empty()) {
        throw Failure("arguments",
                      "Shader, artifact, lifecycle and failure options require --cases", 2);
    }
    return options;
}

std::string versionString(std::uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." +
           std::to_string(VK_API_VERSION_MINOR(version)) + "." +
           std::to_string(VK_API_VERSION_PATCH(version));
}

std::string deviceUuid(const std::uint8_t* bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(VK_UUID_SIZE * 2);
    for (std::size_t index = 0; index < VK_UUID_SIZE; ++index) {
        result.push_back(digits[bytes[index] >> 4U]);
        result.push_back(digits[bytes[index] & 0xfU]);
    }
    return result;
}

void requireVk(VkResult result, std::string_view operation, int exitCode = 1) {
    if (result != VK_SUCCESS) {
        throw Failure("vulkan",
                      std::string(operation) + " failed with VkResult " + std::to_string(result),
                      exitCode);
    }
}

template <class Value, class Enumerate>
std::vector<Value> enumerate(Enumerate operation, std::string_view name) {
    std::uint32_t count = 0;
    requireVk(operation(&count, static_cast<Value*>(nullptr)), name);
    if (count > 4096) {
        throw Failure("enumeration_limit",
                      std::string(name) + " exceeds the probe's 4096-entry safety limit");
    }
    std::vector<Value> values(count);
    if (count != 0) {
        requireVk(operation(&count, values.data()), name);
        if (count > values.size()) {
            throw Failure("enumeration_changed",
                          std::string(name) + " changed during enumeration; rerun explicitly");
        }
        values.resize(count);
    }
    return values;
}

bool containsExtension(const std::vector<VkExtensionProperties>& extensions,
                       std::string_view name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const auto& value) { return value.extensionName == name; });
}

Array extensionReport(const std::vector<VkExtensionProperties>& extensions) {
    Array result;
    for (const auto& extension : extensions) {
        result.emplace_back(
            Object{{"name", extension.extensionName}, {"spec_version", extension.specVersion}});
    }
    return result;
}

Array stringReport(const std::vector<const char*>& strings) {
    Array result;
    for (const char* value : strings) {
        result.emplace_back(value);
    }
    return result;
}

struct Diagnostics final : nvrhi::IMessageCallback {
    std::atomic<std::uint32_t> warnings{0};
    std::atomic<std::uint32_t> errors{0};
    std::atomic<std::uint32_t> emitted{0};

    void record(const char* source, bool error, const char* message) noexcept {
        if (error) {
            ++errors;
        } else {
            ++warnings;
        }
        if (emitted.fetch_add(1) < 256) {
            std::fprintf(stderr, "[%s %s] %s\n", source, error ? "error" : "warning",
                         message != nullptr ? message : "(no message)");
        }
    }

    void message(nvrhi::MessageSeverity severity, const char* messageText) noexcept override {
        if (severity == nvrhi::MessageSeverity::Warning ||
            severity == nvrhi::MessageSeverity::Error ||
            severity == nvrhi::MessageSeverity::Fatal) {
            record("NVRHI", severity != nvrhi::MessageSeverity::Warning, messageText);
        }
    }
};

VKAPI_ATTR VkBool32 VKAPI_CALL validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                 void* user) noexcept {
    auto* diagnostics = static_cast<Diagnostics*>(user);
    diagnostics->record("Vulkan", (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0,
                        data != nullptr ? data->pMessage : nullptr);
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debugInfo(Diagnostics& diagnostics) {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = validationMessage;
    info.pUserData = &diagnostics;
    return info;
}

void glfwError(int code, const char* description) noexcept {
    std::fprintf(stderr, "[GLFW %d] %s\n", code,
                 description != nullptr ? description : "(no description)");
}

struct Context final {
    Diagnostics* diagnostics = nullptr;
    bool glfwInitialized = false;
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    nvrhi::vulkan::DeviceHandle backend;
    nvrhi::DeviceHandle validation;

    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context() noexcept {
        if (device != VK_NULL_HANDLE) {
            // Bounded by the owning process runner even if a broken driver stalls.
            const auto result = vkDeviceWaitIdle(device);
            if (result != VK_SUCCESS) {
                std::fprintf(stderr, "[cleanup] vkDeviceWaitIdle: %d\n", static_cast<int>(result));
                if (diagnostics != nullptr)
                    diagnostics->record("cleanup", true, "vkDeviceWaitIdle failed during teardown");
            }
        }
        validation = nullptr;
        backend = nullptr;
        if (swapchain != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        }
        if (device != VK_NULL_HANDLE) {
            vkDestroyDevice(device, nullptr);
        }
        if (surface != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance, surface, nullptr);
        }
        if (messenger != VK_NULL_HANDLE && destroyMessenger != nullptr) {
            destroyMessenger(instance, messenger, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        if (glfwInitialized) {
            glfwTerminate();
        }
    }
};

void stage(Array& stages, const char* name) {
    stages.emplace_back(Object{{"name", name}, {"status", "PASS"}});
}

std::vector<const char*> initialize(Context& context, Diagnostics& diagnostics, bool validation,
                                    Object& report, Array& stages) {
    context.diagnostics = &diagnostics;
    std::uint32_t loaderVersion = VK_API_VERSION_1_0;
    const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (enumerateVersion != nullptr) {
        requireVk(enumerateVersion(&loaderVersion), "vkEnumerateInstanceVersion", 3);
    }
    report["loader_api_version"] = versionString(loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_3) {
        throw Failure("vulkan_version",
                      "A Vulkan 1.3 loader is required; install the selected driver/loader", 3);
    }
    stage(stages, "loader");

    const auto extensions = enumerate<VkExtensionProperties>(
        [](auto* count, auto* values) {
            return vkEnumerateInstanceExtensionProperties(nullptr, count, values);
        },
        "vkEnumerateInstanceExtensionProperties");
    const auto layers = enumerate<VkLayerProperties>(
        [](auto* count, auto* values) { return vkEnumerateInstanceLayerProperties(count, values); },
        "vkEnumerateInstanceLayerProperties");
    report["instance_extensions"] = extensionReport(extensions);
    Array layerReport;
    bool validationAvailable = false;
    for (const auto& layer : layers) {
        layerReport.emplace_back(Object{{"name", layer.layerName},
                                        {"spec_version", layer.specVersion},
                                        {"implementation_version", layer.implementationVersion}});
        validationAvailable = validationAvailable ||
                              std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
    }
    report["instance_layers"] = std::move(layerReport);
    report["vulkan_validation_requested"] = validation;
    if (validation && (!validationAvailable ||
                       !containsExtension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME))) {
        throw Failure("validation_unavailable",
                      "Provide VK_LAYER_KHRONOS_validation and its manifest via VK_LAYER_PATH; "
                      "required layer/debug-utils support is unavailable. The layer package must "
                      "be pinned in 02b before qualification. --validation off is diagnostic only.",
                      3);
    }

    glfwSetErrorCallback(glfwError);
    glfwInitVulkanLoader(vkGetInstanceProcAddr);
#if defined(__linux__)
    // The accepted Linux profile is native X11, not a silent Wayland fallback.
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    if (glfwInit() != GLFW_TRUE) {
        throw Failure(
            "display_unavailable",
            "GLFW could not initialize a desktop session; provide a native supported display", 3);
    }
    context.glfwInitialized = true;
    std::uint32_t requiredCount = 0;
    const char** requiredExtensions = glfwGetRequiredInstanceExtensions(&requiredCount);
    if (requiredExtensions == nullptr || requiredCount == 0) {
        throw Failure("surface_extensions_unavailable",
                      "GLFW has no Vulkan surface route on this display/loader", 3);
    }
    std::vector<const char*> enabled(requiredExtensions, requiredExtensions + requiredCount);
#if defined(__APPLE__)
    if (std::none_of(enabled.begin(), enabled.end(), [](const char* extension) {
            return std::string_view(extension) == "VK_EXT_metal_surface";
        })) {
        throw Failure("metal_surface_unavailable",
                      "The Mac probe requires GLFW's modern VK_EXT_metal_surface route; a legacy "
                      "surface extension cannot qualify this setup",
                      3);
    }
#endif
    for (const char* extension : enabled) {
        if (!containsExtension(extensions, extension)) {
            throw Failure("surface_extension_unavailable",
                          "Required GLFW Vulkan instance extension is unavailable: " +
                              std::string(extension),
                          3);
        }
    }
    const bool portability =
        containsExtension(extensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (portability) {
        enabled.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    if (validation) {
        enabled.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        const auto layerExtensions = enumerate<VkExtensionProperties>(
            [](auto* count, auto* values) {
                return vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation", count,
                                                              values);
            },
            "validation layer extensions");
        if (!containsExtension(layerExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
            throw Failure(
                "validation_unavailable",
                "Pinned layer lacks VK_EXT_validation_features for synchronization validation", 3);
        }
        enabled.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Shark GraphicsProbe 02b";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.pEngineName = "Shark probe (no renderer)";
    application.apiVersion = VK_API_VERSION_1_3;
    const char* layerName = "VK_LAYER_KHRONOS_validation";
    auto debug = debugInfo(diagnostics);
    constexpr VkValidationFeatureEnableEXT syncValidation =
        VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validationFeatures{};
    validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    validationFeatures.pNext = &debug;
    validationFeatures.enabledValidationFeatureCount = 1;
    validationFeatures.pEnabledValidationFeatures = &syncValidation;
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = validation ? &validationFeatures : nullptr;
    instanceInfo.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    instanceInfo.ppEnabledExtensionNames = enabled.data();
    instanceInfo.enabledLayerCount = validation ? 1U : 0U;
    instanceInfo.ppEnabledLayerNames = validation ? &layerName : nullptr;
    requireVk(vkCreateInstance(&instanceInfo, nullptr, &context.instance), "vkCreateInstance", 3);
    report["vulkan_validation_enabled"] = validation;
    report["validation_configuration"] = Object{{"core", validation},
                                                {"synchronization", validation},
                                                {"gpu_assisted", false},
                                                {"disabled_checks", Array{}}};
    stage(stages, "instance");
    report["enabled_instance_extensions"] = stringReport(enabled);
    report["portability_enumeration_enabled"] = portability;
    if (validation) {
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(context.instance, "vkCreateDebugUtilsMessengerEXT"));
        context.destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(context.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (createMessenger == nullptr || context.destroyMessenger == nullptr) {
            throw Failure("validation_dispatch",
                          "Enabled debug-utils extension has missing create/destroy entry points");
        }
        requireVk(createMessenger(context.instance, &debug, nullptr, &context.messenger),
                  "vkCreateDebugUtilsMessengerEXT");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    context.window = glfwCreateWindow(64, 64, "Shark GraphicsProbe 02b", nullptr, nullptr);
    if (context.window == nullptr) {
        throw Failure("window_unavailable", "GLFW could not create the hidden 64x64 probe window",
                      3);
    }
    requireVk(glfwCreateWindowSurface(context.instance, context.window, nullptr, &context.surface),
              "glfwCreateWindowSurface", 3);
    stage(stages, "surface");
    report["glfw_version"] = glfwGetVersionString();
    report["glfw_platform"] = static_cast<std::uint32_t>(glfwGetPlatform());
    return enabled;
}

struct FormatRequirement final {
    const char* name;
    VkFormat format;
    VkFormatFeatureFlags features;
    VkImageUsageFlags usage;
};

constexpr VkFormatFeatureFlags sampled =
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
constexpr VkFormatFeatureFlags transfer =
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
constexpr VkFormatFeatureFlags color =
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
constexpr VkImageUsageFlags upload =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr std::array formatRequirements{
    FormatRequirement{"R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB, sampled | transfer, upload},
    FormatRequirement{"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, sampled | transfer | color,
                      upload | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
    FormatRequirement{"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT,
                      sampled | transfer | color | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                      upload | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT},
    FormatRequirement{"D32_SFLOAT", VK_FORMAT_D32_SFLOAT,
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                          VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}};

struct DeviceInfo final {
    VkPhysicalDevice handle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    std::string uuid;
    std::vector<VkExtensionProperties> extensions;
    std::optional<std::uint32_t> queue;
    std::vector<std::string> missing;
    Object report;
};

void missingIf(DeviceInfo& device, bool condition, std::string reason) {
    if (condition) {
        device.missing.push_back(std::move(reason));
    }
}

DeviceInfo inspectDevice(VkPhysicalDevice handle, VkSurfaceKHR surface) {
    DeviceInfo device;
    device.handle = handle;
    vkGetPhysicalDeviceProperties(handle, &device.properties);
    const auto api = device.properties.apiVersion;
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    identity.pNext = api >= VK_API_VERSION_1_2 ? &driver : nullptr;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    vkGetPhysicalDeviceProperties2(handle, &properties);
    device.uuid = deviceUuid(identity.deviceUUID);
    device.extensions = enumerate<VkExtensionProperties>(
        [handle](auto* count, auto* values) {
            return vkEnumerateDeviceExtensionProperties(handle, nullptr, count, values);
        },
        "vkEnumerateDeviceExtensionProperties");
    const bool portability =
        containsExtension(device.extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);

    VkPhysicalDevicePortabilitySubsetFeaturesKHR portableFeatures{};
    portableFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR;
    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features13.pNext = portability ? &portableFeatures : nullptr;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = api >= VK_API_VERSION_1_3 ? static_cast<void*>(&features13)
                                                 : (portability ? &portableFeatures : nullptr);
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = api >= VK_API_VERSION_1_2 ? static_cast<void*>(&features12)
                                               : (portability ? &portableFeatures : nullptr);
    vkGetPhysicalDeviceFeatures2(handle, &features);
    device.report = Object{
        {"name", device.properties.deviceName},
        {"uuid", device.uuid},
        {"vendor_id", device.properties.vendorID},
        {"device_id", device.properties.deviceID},
        {"device_type", static_cast<std::uint32_t>(device.properties.deviceType)},
        {"api_version", versionString(api)},
        {"driver_version_raw", device.properties.driverVersion},
        {"driver_name", driver.driverName},
        {"driver_info", driver.driverInfo},
        {"driver_id", static_cast<std::uint32_t>(driver.driverID)},
        {"extensions", extensionReport(device.extensions)},
        {"portability_subset", portability},
        {"features", Object{{"timelineSemaphore", features12.timelineSemaphore == VK_TRUE},
                            {"synchronization2", features13.synchronization2 == VK_TRUE},
                            {"dynamicRendering", features13.dynamicRendering == VK_TRUE},
                            {"bufferDeviceAddress", features12.bufferDeviceAddress == VK_TRUE},
                            {"samplerAnisotropy", features.features.samplerAnisotropy == VK_TRUE},
                            {"shaderStorageImageExtendedFormats",
                             features.features.shaderStorageImageExtendedFormats == VK_TRUE}}}};
    if (portability) {
        VkPhysicalDevicePortabilitySubsetPropertiesKHR portableProperties{};
        portableProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_PROPERTIES_KHR;
        properties.pNext = &portableProperties;
        vkGetPhysicalDeviceProperties2(handle, &properties);
        device.report["portability_restrictions"] =
            Object{{"events", portableFeatures.events == VK_TRUE},
                   {"imageViewFormatReinterpretation",
                    portableFeatures.imageViewFormatReinterpretation == VK_TRUE},
                   {"imageViewFormatSwizzle", portableFeatures.imageViewFormatSwizzle == VK_TRUE},
                   {"samplerMipLodBias", portableFeatures.samplerMipLodBias == VK_TRUE},
                   {"separateStencilMaskRef", portableFeatures.separateStencilMaskRef == VK_TRUE},
                   {"constantAlphaColorBlendFactors",
                    portableFeatures.constantAlphaColorBlendFactors == VK_TRUE},
                   {"vertexAttributeAccessBeyondStride",
                    portableFeatures.vertexAttributeAccessBeyondStride == VK_TRUE},
                   {"minVertexInputBindingStrideAlignment",
                    portableProperties.minVertexInputBindingStrideAlignment}};
    }
    missingIf(device, api < VK_API_VERSION_1_3, "Vulkan API 1.3");
    missingIf(device, features12.timelineSemaphore != VK_TRUE, "timelineSemaphore");
    missingIf(device, features13.synchronization2 != VK_TRUE, "synchronization2");
    missingIf(device, features13.dynamicRendering != VK_TRUE, "dynamicRendering");
    missingIf(device, !containsExtension(device.extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME),
              "VK_KHR_swapchain");

    const auto& limits = device.properties.limits;
    device.report["limits"] =
        Object{{"maxImageDimension2D", limits.maxImageDimension2D},
               {"maxImageDimensionCube", limits.maxImageDimensionCube},
               {"maxImageArrayLayers", limits.maxImageArrayLayers},
               {"maxColorAttachments", limits.maxColorAttachments},
               {"maxBoundDescriptorSets", limits.maxBoundDescriptorSets},
               {"maxPerStageDescriptorSampledImages", limits.maxPerStageDescriptorSampledImages},
               {"maxPerStageDescriptorSamplers", limits.maxPerStageDescriptorSamplers},
               {"maxUniformBufferRange", limits.maxUniformBufferRange},
               {"maxStorageBufferRange", limits.maxStorageBufferRange},
               {"maxComputeWorkGroupInvocations", limits.maxComputeWorkGroupInvocations}};
    missingIf(device, limits.maxImageDimension2D < 4096, "maxImageDimension2D >= 4096");
    missingIf(device, limits.maxImageDimensionCube < 2048, "maxImageDimensionCube >= 2048");
    missingIf(device, limits.maxImageArrayLayers < 12, "maxImageArrayLayers >= 12");
    missingIf(device, limits.maxColorAttachments < 4, "maxColorAttachments >= 4");
    missingIf(device, limits.maxBoundDescriptorSets < 4, "maxBoundDescriptorSets >= 4");
    missingIf(device, limits.maxPerStageDescriptorSampledImages < 16,
              "maxPerStageDescriptorSampledImages >= 16");
    missingIf(device, limits.maxPerStageDescriptorSamplers < 16,
              "maxPerStageDescriptorSamplers >= 16");
    missingIf(device, limits.maxUniformBufferRange < 16384, "maxUniformBufferRange >= 16384");
    missingIf(device, limits.maxStorageBufferRange < 134217728,
              "maxStorageBufferRange >= 134217728");
    missingIf(device, limits.maxComputeWorkGroupInvocations < 128,
              "maxComputeWorkGroupInvocations >= 128");

    std::uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(handle, &queueCount, nullptr);
    if (queueCount == 0 || queueCount > 4096) {
        throw Failure("queue_enumeration",
                      "Physical device has an invalid or excessive queue-family count");
    }
    std::vector<VkQueueFamilyProperties> queues(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(handle, &queueCount, queues.data());
    Array queueReport;
    for (std::uint32_t index = 0; index < queueCount; ++index) {
        VkBool32 present = VK_FALSE;
        requireVk(vkGetPhysicalDeviceSurfaceSupportKHR(handle, index, surface, &present),
                  "vkGetPhysicalDeviceSurfaceSupportKHR");
        const auto& queue = queues[index];
        constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if (!device.queue && queue.queueCount > 0 && (queue.queueFlags & required) == required &&
            present == VK_TRUE) {
            device.queue = index;
        }
        queueReport.emplace_back(Object{{"index", index},
                                        {"count", queue.queueCount},
                                        {"flags", queue.queueFlags},
                                        {"present", present == VK_TRUE}});
    }
    missingIf(device, !device.queue, "one graphics+compute+present queue family");
    device.report["queues"] = std::move(queueReport);

    Array formatReport;
    for (const auto& required : formatRequirements) {
        VkFormatProperties formatProperties{};
        vkGetPhysicalDeviceFormatProperties(handle, required.format, &formatProperties);
        VkImageFormatProperties imageProperties{};
        const VkResult result = vkGetPhysicalDeviceImageFormatProperties(
            handle, required.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, required.usage, 0,
            &imageProperties);
        if (result != VK_SUCCESS && result != VK_ERROR_FORMAT_NOT_SUPPORTED) {
            requireVk(result, "vkGetPhysicalDeviceImageFormatProperties");
        }
        const bool featuresSupported =
            (formatProperties.optimalTilingFeatures & required.features) == required.features;
        const bool depthArraySupported =
            required.format != VK_FORMAT_D32_SFLOAT ||
            (imageProperties.maxExtent.width >= 2048 && imageProperties.maxExtent.height >= 2048 &&
             imageProperties.maxArrayLayers >= 12);
        const bool supported = result == VK_SUCCESS && featuresSupported && depthArraySupported;
        formatReport.emplace_back(Object{
            {"name", required.name},
            {"format", static_cast<std::uint32_t>(required.format)},
            {"optimal_tiling_features", formatProperties.optimalTilingFeatures},
            {"required_features", required.features},
            {"required_usage", required.usage},
            {"image_usage_supported", result == VK_SUCCESS},
            {"max_extent", Array{imageProperties.maxExtent.width, imageProperties.maxExtent.height,
                                 imageProperties.maxExtent.depth}},
            {"max_array_layers", imageProperties.maxArrayLayers},
            {"max_mip_levels", imageProperties.maxMipLevels},
            {"max_resource_size", static_cast<std::uint64_t>(imageProperties.maxResourceSize)},
            {"reported_profile_supported", supported},
            {"execution", "NOT_RUN"}});
        missingIf(
            device, !supported,
            std::string(required.name) +
                " required optimal-tiling features/combined image usage or depth-array extent");
    }
    device.report["formats"] = std::move(formatReport);
    const auto surfaceFormats = enumerate<VkSurfaceFormatKHR>(
        [handle, surface](auto* count, auto* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(handle, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfaceFormatsKHR");
    Array surfaceReport;
    bool srgb = false;
    for (const auto& format : surfaceFormats) {
        surfaceReport.emplace_back(
            Object{{"format", static_cast<std::uint32_t>(format.format)},
                   {"color_space", static_cast<std::uint32_t>(format.colorSpace)}});
        srgb = srgb || ((format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                         format.format == VK_FORMAT_R8G8B8A8_SRGB ||
                         format.format == VK_FORMAT_UNDEFINED) &&
                        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
    }
    device.report["surface_formats"] = std::move(surfaceReport);
    missingIf(device, !srgb,
              "sRGB presentation format (UNORM encoding fallback is unimplemented in 02a)");
    const auto presentModes = enumerate<VkPresentModeKHR>(
        [handle, surface](auto* count, auto* values) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(handle, surface, count, values);
        },
        "vkGetPhysicalDeviceSurfacePresentModesKHR");
    Array modeReport;
    for (const auto mode : presentModes) {
        modeReport.emplace_back(static_cast<std::uint32_t>(mode));
    }
    device.report["present_modes"] = std::move(modeReport);
    missingIf(device,
              std::find(presentModes.begin(), presentModes.end(), VK_PRESENT_MODE_FIFO_KHR) ==
                  presentModes.end(),
              "FIFO presentation");
    Array missing;
    for (const auto& item : device.missing) {
        missing.emplace_back(item);
    }
    device.report["missing_reported_requirements"] = std::move(missing);
    device.report["reported_profile_supported"] = device.missing.empty();
    device.report["execution_qualification"] = "NOT_RUN";
    return device;
}

void smoke(Context& context, Diagnostics& diagnostics, const DeviceInfo& selected,
           const std::vector<const char*>& instanceExtensions, Object& report, Array& stages) {
    if (!selected.missing.empty()) {
        std::string missing;
        for (const auto& requirement : selected.missing) {
            missing += (missing.empty() ? "" : "; ") + requirement;
        }
        throw Failure("capabilities_missing",
                      "Selected device reports missing profile requirements: " + missing);
    }
    const std::uint32_t queueFamily = *selected.queue;
    const float priority = 1.0F;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features enabled13{};
    enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    enabled13.synchronization2 = VK_TRUE;
    enabled13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enabled12{};
    enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    enabled12.pNext = &enabled13;
    enabled12.timelineSemaphore = VK_TRUE;
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (containsExtension(selected.extensions, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    }
    // Pinned NVRHI startup creates timeline semaphores. No ray tracing,
    // descriptor indexing or buffer-device-address feature is needed here.
    // Draw/compute paths require their own feature audit in 02b.
    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = &enabled12;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    requireVk(vkCreateDevice(selected.handle, &deviceInfo, nullptr, &context.device),
              "vkCreateDevice");
    stage(stages, "device");
    report["enabled_device_extensions"] = stringReport(extensions);
    report["enabled_features"] = Array{"timelineSemaphore", "synchronization2", "dynamicRendering"};
    report["selected_queue_family"] = queueFamily;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(context.device, queueFamily, 0, &queue);
    context.queue = queue;
    context.queueFamily = queueFamily;
    VULKAN_HPP_DEFAULT_DISPATCHER.init(context.instance, vkGetInstanceProcAddr, context.device);
    nvrhi::vulkan::DeviceDesc nvrhiInfo{};
    nvrhiInfo.errorCB = &diagnostics;
    nvrhiInfo.instance = context.instance;
    nvrhiInfo.physicalDevice = selected.handle;
    nvrhiInfo.device = context.device;
    nvrhiInfo.graphicsQueue = queue;
    nvrhiInfo.graphicsQueueIndex = static_cast<int>(queueFamily);
    nvrhiInfo.instanceExtensions = const_cast<const char**>(instanceExtensions.data());
    nvrhiInfo.numInstanceExtensions = instanceExtensions.size();
    nvrhiInfo.deviceExtensions = extensions.data();
    nvrhiInfo.numDeviceExtensions = extensions.size();
    context.backend = nvrhi::vulkan::createDevice(nvrhiInfo);
    if (!context.backend) {
        throw Failure("nvrhi_device", "NVRHI Vulkan device initialization returned null");
    }
    context.validation = nvrhi::validation::createValidationLayer(context.backend);
    if (!context.validation) {
        throw Failure("nvrhi_validation", "NVRHI validation wrapper initialization returned null");
    }
    stage(stages, "nvrhi_device_and_validation");
    report["nvrhi_validation_enabled"] = true;
}

void createSwapchain(Context& context, const DeviceInfo& selected, Object& report, Array& stages) {
    VkSurfaceCapabilitiesKHR capabilities{};
    requireVk(
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(selected.handle, context.surface, &capabilities),
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    const auto formats = enumerate<VkSurfaceFormatKHR>(
        [&selected, &context](auto* count, auto* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(selected.handle, context.surface, count,
                                                        values);
        },
        "vkGetPhysicalDeviceSurfaceFormatsKHR");
    const auto found = std::find_if(formats.begin(), formats.end(), [](const auto& format) {
        return (format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                format.format == VK_FORMAT_R8G8B8A8_SRGB || format.format == VK_FORMAT_UNDEFINED) &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    if (found == formats.end()) {
        throw Failure("surface_changed", "Surface lost the previously reported sRGB format");
    }
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(context.window, &width, &height);
        if (width <= 0 || height <= 0) {
            throw Failure("surface_zero_extent",
                          "Probe window has zero drawable framebuffer extent", 3);
        }
        extent.width =
            std::clamp(static_cast<std::uint32_t>(width), capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width);
        extent.height =
            std::clamp(static_cast<std::uint32_t>(height), capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0 ||
        (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
        throw Failure("surface_unavailable",
                      "Probe surface has zero extent or lacks color-attachment usage", 3);
    }
    constexpr std::array alphaModes{
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    const auto alpha =
        std::find_if(alphaModes.begin(), alphaModes.end(), [&capabilities](const auto mode) {
            return (capabilities.supportedCompositeAlpha &
                    static_cast<VkCompositeAlphaFlagsKHR>(mode)) != 0;
        });
    if (alpha == alphaModes.end()) {
        throw Failure("surface_alpha", "Surface has no supported composite-alpha mode");
    }
    std::uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount != 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }
    VkSwapchainCreateInfoKHR swapchainInfo{};
    swapchainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    swapchainInfo.surface = context.surface;
    swapchainInfo.minImageCount = imageCount;
    swapchainInfo.imageFormat =
        found->format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_SRGB : found->format;
    swapchainInfo.imageColorSpace = found->colorSpace;
    swapchainInfo.imageExtent = extent;
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchainInfo.preTransform = capabilities.currentTransform;
    swapchainInfo.compositeAlpha = *alpha;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    requireVk(vkCreateSwapchainKHR(context.device, &swapchainInfo, nullptr, &context.swapchain),
              "vkCreateSwapchainKHR");
    const auto images = enumerate<VkImage>(
        [&context](auto* count, auto* values) {
            return vkGetSwapchainImagesKHR(context.device, context.swapchain, count, values);
        },
        "vkGetSwapchainImagesKHR");
    context.swapchainFormat = swapchainInfo.imageFormat;
    context.swapchainExtent = extent;
    context.swapchainImages = images;
    if (images.empty()) {
        throw Failure("swapchain_images", "Created swapchain has no images");
    }
    stage(stages, "swapchain");
    report["swapchain"] = Object{
        {"format", static_cast<std::uint32_t>(swapchainInfo.imageFormat)},
        {"color_space", static_cast<std::uint32_t>(swapchainInfo.imageColorSpace)},
        {"extent", Array{extent.width, extent.height}},
        {"image_count", static_cast<std::uint64_t>(images.size())},
        {"present_mode", "FIFO"},
        {"snapshot", "initial creation before submission; aggregate frames are in lifecycle"},
        {"presented_frames", 0U}};
}

// This probe deliberately serializes submissions and waits before releasing any
// submitted resource. It is a qualification fixture, not a production frame loop.
void presentFrame(Context& context) {
    int width = 0, height = 0;
    glfwGetFramebufferSize(context.window, &width, &height);
    if (!shark::probe::drawableFramebuffer(
            width, height, glfwGetWindowAttrib(context.window, GLFW_ICONIFIED) == GLFW_TRUE)) {
        throw Failure("framebuffer_unavailable",
                      "Presentation is suspended for a minimized/zero framebuffer", 3);
    }
    struct Frame final {
        VkDevice device;
        VkFence acquired = VK_NULL_HANDLE;
        VkFence completed = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkRenderPass pass = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        ~Frame() noexcept {
            (void)vkDeviceWaitIdle(device);
            if (framebuffer)
                vkDestroyFramebuffer(device, framebuffer, nullptr);
            if (pass)
                vkDestroyRenderPass(device, pass, nullptr);
            if (view)
                vkDestroyImageView(device, view, nullptr);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            if (completed)
                vkDestroyFence(device, completed, nullptr);
            if (acquired)
                vkDestroyFence(device, acquired, nullptr);
        }
    } frame{context.device};
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    requireVk(vkCreateFence(context.device, &fenceInfo, nullptr, &frame.acquired),
              "create acquire fence");
    requireVk(vkCreateFence(context.device, &fenceInfo, nullptr, &frame.completed),
              "create submit fence");
    std::uint32_t imageIndex = 0;
    const auto acquire = vkAcquireNextImageKHR(context.device, context.swapchain, 5'000'000'000ULL,
                                               VK_NULL_HANDLE, frame.acquired, &imageIndex);
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
        requireVk(acquire, "acquire presentation image");
    requireVk(vkWaitForFences(context.device, 1, &frame.acquired, VK_TRUE, 5'000'000'000ULL),
              "wait acquire fence");
    if (imageIndex >= context.swapchainImages.size())
        throw Failure("swapchain_image", "Acquired invalid image index");
    VkImageViewCreateInfo view{};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = context.swapchainImages[imageIndex];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = context.swapchainFormat;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    requireVk(vkCreateImageView(context.device, &view, nullptr, &frame.view),
              "create presentation view");
    VkAttachmentDescription attachment{};
    attachment.format = context.swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass{};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    pass.attachmentCount = 1;
    pass.pAttachments = &attachment;
    pass.subpassCount = 1;
    pass.pSubpasses = &subpass;
    pass.dependencyCount = 1;
    pass.pDependencies = &dependency;
    requireVk(vkCreateRenderPass(context.device, &pass, nullptr, &frame.pass),
              "create presentation render pass");
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = frame.pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &frame.view;
    fb.width = context.swapchainExtent.width;
    fb.height = context.swapchainExtent.height;
    fb.layers = 1;
    requireVk(vkCreateFramebuffer(context.device, &fb, nullptr, &frame.framebuffer),
              "create presentation framebuffer");
    // The command pool uses the same family selected for NVRHI and presentation.
    VkCommandPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.queueFamilyIndex = context.queueFamily;
    requireVk(vkCreateCommandPool(context.device, &pool, nullptr, &frame.pool),
              "create presentation command pool");
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = frame.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    requireVk(vkAllocateCommandBuffers(context.device, &allocate, &command),
              "allocate presentation command buffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    requireVk(vkBeginCommandBuffer(command, &begin), "begin presentation commands");
    VkClearValue clear{};
    clear.color.float32[0] = 0.125F;
    clear.color.float32[1] = 0.25F;
    clear.color.float32[2] = 0.5F;
    clear.color.float32[3] = 1.0F;
    VkRenderPassBeginInfo render{};
    render.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render.renderPass = frame.pass;
    render.framebuffer = frame.framebuffer;
    render.renderArea.extent = context.swapchainExtent;
    render.clearValueCount = 1;
    render.pClearValues = &clear;
    vkCmdBeginRenderPass(command, &render, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdEndRenderPass(command);
    requireVk(vkEndCommandBuffer(command), "end presentation commands");
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    requireVk(vkQueueSubmit(context.queue, 1, &submit, frame.completed),
              "submit presentation commands");
    requireVk(vkWaitForFences(context.device, 1, &frame.completed, VK_TRUE, 5'000'000'000ULL),
              "wait presentation commands");
    VkPresentInfoKHR present{};
    present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present.swapchainCount = 1;
    present.pSwapchains = &context.swapchain;
    present.pImageIndices = &imageIndex;
    const auto presented = vkQueuePresentKHR(context.queue, &present);
    if (presented != VK_SUCCESS && presented != VK_SUBOPTIMAL_KHR)
        requireVk(presented, "present frame");
    requireVk(vkQueueWaitIdle(context.queue), "retire presentation queue");
}

struct LifecycleResults final {
    std::uint32_t resizeCycles = 0;
    std::uint32_t minimized = 0;
    std::uint32_t zeroExtents = 0;
    std::uint32_t frames = 0;
    double seconds = 0;
};

template <class Predicate> void awaitWindow(Predicate predicate, const char* operation) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw Failure("window_timeout", operation);
        glfwWaitEventsTimeout(0.01);
    }
}

LifecycleResults exerciseLifecycle(Context& context, const DeviceInfo& selected,
                                   const Options& options) {
    LifecycleResults result;
    if (!context.validation->waitForIdle())
        throw Failure("device_wait", "NVRHI failed to retire readback work");
    context.validation->runGarbageCollection();
    auto recreate = [&] {
        requireVk(vkDeviceWaitIdle(context.device), "wait before swapchain recreation");
        vkDestroySwapchainKHR(context.device, context.swapchain, nullptr);
        context.swapchain = VK_NULL_HANDLE;
        context.swapchainImages.clear();
        Object ignoredReport;
        Array ignoredStages;
        createSwapchain(context, selected, ignoredReport, ignoredStages);
    };
    glfwShowWindow(context.window);
    glfwPollEvents();
    presentFrame(context);
    ++result.frames;
    for (std::uint32_t cycle = 0; cycle < options.lifecycleCycles; ++cycle) {
        // Stay above native window-manager minimum tracking sizes; tiny windows
        // can be clamped even when glfwSetWindowSize is otherwise successful.
        const int size = cycle % 2 == 0 ? 256 : 320;
        glfwSetWindowSize(context.window, size, size);
        int observedWidth = 0, observedHeight = 0;
        try {
            awaitWindow(
                [&] {
                    glfwGetWindowSize(context.window, &observedWidth, &observedHeight);
                    return observedWidth == size && observedHeight == size;
                },
                "Resize was not observed within five seconds");
        } catch (const Failure& failure) {
            throw Failure(failure.code,
                          std::string(failure.what()) + "; cycle=" + std::to_string(cycle) +
                              "; requested=" + std::to_string(size) + "; observed=" +
                              std::to_string(observedWidth) + "x" + std::to_string(observedHeight));
        }
        recreate();
        presentFrame(context);
        ++result.frames;
        glfwIconifyWindow(context.window);
        awaitWindow(
            [&] { return glfwGetWindowAttrib(context.window, GLFW_ICONIFIED) == GLFW_TRUE; },
            "Minimize was not observed within five seconds");
        ++result.minimized;
        int width = 0, height = 0;
        glfwGetFramebufferSize(context.window, &width, &height);
        if (width == 0 || height == 0)
            ++result.zeroExtents;
        // No acquire/present/recreate while minimized, including a zero framebuffer.
        glfwRestoreWindow(context.window);
        awaitWindow(
            [&] {
                int restoredWidth = 0, restoredHeight = 0;
                int framebufferWidth = 0, framebufferHeight = 0;
                glfwGetWindowSize(context.window, &restoredWidth, &restoredHeight);
                glfwGetFramebufferSize(context.window, &framebufferWidth, &framebufferHeight);
                // The show-state flag alone does not establish restored dimensions.
                return glfwGetWindowAttrib(context.window, GLFW_ICONIFIED) == GLFW_FALSE &&
                       restoredWidth == size && restoredHeight == size && framebufferWidth > 0 &&
                       framebufferHeight > 0;
            },
            "Restore dimensions/framebuffer were not observed within five seconds");
        recreate();
        presentFrame(context);
        ++result.frames;
        ++result.resizeCycles;
    }
    const auto started = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - started <
           std::chrono::seconds(options.presentSeconds)) {
        glfwPollEvents();
        if (glfwWindowShouldClose(context.window))
            throw Failure("window_closed",
                          "Probe window closed before requested presentation duration");
        if (glfwGetWindowAttrib(context.window, GLFW_ICONIFIED) == GLFW_TRUE)
            throw Failure("unexpected_minimize",
                          "Window minimized during timed presentation; rerun explicitly");
        presentFrame(context);
        ++result.frames;
    }
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return result;
}

Object baseReport() {
    Array cases;
    constexpr std::array names{"stack_device",          "graphics_depth", "compute",
                               "formats_shaders",       "lifecycle",      "validation",
                               "portability_packaging", "failure_paths"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        cases.emplace_back(Object{
            {"id", "P02-0" + std::to_string(index + 1)},
            {"name", names[index]},
            {"status", "NOT_RUN"},
            {"execution_status", "NOT_RUN"},
            {"reason",
             "Case not executed in this operation; full matrix qualification remains open"}});
    }
    return Object{{"schema_version", 2U},
                  {"probe_version", SHARK_PROBE_VERSION},
                  {"build_compiler", SHARK_PROBE_COMPILER},
                  {"dependency_lock_sha256", SHARK_PROBE_LOCK_SHA256},
                  {"probe_source_sha256", SHARK_PROBE_SOURCE_SHA256},
                  {"mode", "invalid"},
                  {"status", "FAIL"},
                  {"qualification", "NOT_RUN"},
                  {"cases", std::move(cases)},
                  {"stages", Array{}},
                  {"devices", Array{}},
                  {"selected_device_uuid", Json{}},
                  {"vulkan_validation_enabled", false},
                  {"nvrhi_validation_enabled", false},
                  {"validation_warnings", 0U},
                  {"validation_errors", 0U}};
}

void describe(Object& report) {
    report["status"] = "PASS";
    report["description"] =
        "Maintained graphics feasibility probe with NVRHI draw/compute/readback cases. "
        "This is not a production renderer or game-authoring API.";
    report["commands"] = Array{
        Object{{"arguments", "--describe"},
               {"device_required", false},
               {"result", "machine-readable discovery; no GLFW/Vulkan initialization"}},
        Object{{"arguments", "--inventory"},
               {"device_required", true},
               {"result",
                "hidden surface and physical-device capability inventory; no logical device"}},
        Object{{"arguments", "--smoke --device <UUID> [--validation required|off]"},
               {"device_required", true},
               {"result", "explicit device, NVRHI validation wrapper and swapchain create/destroy; "
                          "no submitted work"}},
        Object{
            {"arguments", "--cases --device <UUID> --shader-dir <path> --artifacts-dir <newpath>"},
            {"device_required", true},
            {"result", "NVRHI draw/compute/readback and bounded presentation lifecycle"}}};
    report["exit_codes"] =
        Object{{"0", "requested setup operation completed; qualification remains NOT_RUN"},
               {"1", "observed operation/capability/validation failure"},
               {"2", "invalid arguments; no graphics initialization"},
               {"3", "required environment/device/validation prerequisite unavailable; NOT_RUN"}};
    report["timeout"] = "Use tools/graphics_probe.py for a bounded process deadline; native driver "
                        "calls cannot be cancelled safely in process.";
    report["string_encoding"] =
        "Report strings preserve source bytes as U+0000..U+00ff; ASCII metadata is unchanged.";
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // Keep assertion/device-crash diagnostics noninteractive under the external
    // process watchdog; an agent must never wait for a Windows error dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    _set_error_mode(_OUT_TO_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    Object report = baseReport();
    Array stages;
    Diagnostics diagnostics;
    int exitCode = 0;
    std::vector<Object> executedCases;
    try {
        const Options options = parseOptions(argc, argv);
        report["mode"] = options.mode;
        if (options.mode == "help") {
            std::cout
                << "Shark GraphicsProbe " SHARK_PROBE_VERSION " (02b executable cases)\n"
                   "  --help | --version | --describe\n"
                   "  --inventory\n"
                   "  --smoke --device <32-lowercase-hex-UUID> [--validation required|off]\n"
                   "Discovery/help/version do not initialize graphics. Inventory requires a "
                   "desktop.\n"
                   "Smoke defaults to required Vulkan validation; off is diagnostic only.\n"
                   "Use tools/graphics_probe.py to enforce a process timeout.\n"
                   "  --cases --device <UUID> --shader-dir <path> --artifacts-dir <newpath>\n"
                   "    [--validation required|off] [--lifecycle-cycles 0..100]\n"
                   "    [--device-cycles 1..20] [--present-seconds 0..600]\n"
                   "    [--inject-failure none|missing-feature|shader|readback]\n"
                   "Defaults: 2 lifecycle cycles, 1 device cycle, 0 timed presentation seconds.\n"
                   "Full matrix and packaged-platform qualification remain open.\n";
            return std::cout.good() ? 0 : 1;
        }
        if (options.mode == "version") {
            std::cout << "Shark GraphicsProbe " SHARK_PROBE_VERSION "\n";
            return std::cout.good() ? 0 : 1;
        }
        if (options.mode == "describe") {
            describe(report);
        } else {
            LifecycleResults lifecycle;
            const std::uint32_t cycles = options.mode == "cases" ? options.deviceCycles : 1;
            std::uint32_t completedDevices = 0;
            for (std::uint32_t cycle = 0; cycle < cycles; ++cycle) {
                Array cycleStages;
                Array& currentStages = cycle == 0 ? stages : cycleStages;
                {
                    Context context;
                    const auto instanceExtensions = initialize(
                        context, diagnostics,
                        (options.mode == "smoke" || options.mode == "cases") && options.validation,
                        report, currentStages);
                    const auto devices = enumerate<VkPhysicalDevice>(
                        [&context](auto* count, auto* values) {
                            return vkEnumeratePhysicalDevices(context.instance, count, values);
                        },
                        "vkEnumeratePhysicalDevices");
                    if (devices.empty())
                        throw Failure(
                            "device_unavailable",
                            "No Vulkan physical device found; install the selected GPU driver/ICD",
                            3);
                    Array inventory;
                    std::optional<DeviceInfo> selected;
                    for (const auto handle : devices) {
                        auto device = inspectDevice(handle, context.surface);
                        inventory.emplace_back(device.report);
                        if (device.uuid == options.device) {
                            if (selected)
                                throw Failure("ambiguous_device",
                                              "Multiple physical devices have the requested UUID; "
                                              "fix duplicate ICD discovery");
                            selected = std::move(device);
                        }
                    }
                    report["devices"] = std::move(inventory);
                    stage(currentStages, "inventory");
                    if (options.mode == "smoke" || options.mode == "cases") {
                        report["selected_device_uuid"] = options.device;
                        if (!selected)
                            throw Failure("device_unavailable",
                                          "Requested Vulkan deviceUUID is absent; rerun "
                                          "--inventory and select explicitly",
                                          3);
                        if (options.injectedFailure == "missing-feature") {
                            selected->missing.emplace_back(
                                "timelineSemaphore (explicit missing-feature fixture)");
                        }
                        smoke(context, diagnostics, *selected, instanceExtensions, report,
                              currentStages);
                        createSwapchain(context, *selected, report, currentStages);
                        if (options.mode == "cases") {
                            report["injected_failure"] = options.injectedFailure;
                            if (cycle == 0) {
                                const auto shaderDirectory =
                                    options.injectedFailure == "shader"
                                        ? options.shaderDirectory / "injected-missing-shader"
                                        : options.shaderDirectory;
                                auto readbacks = shark::probe::runReadbackCases(
                                    context.validation, shaderDirectory, options.artifactsDirectory,
                                    options.injectedFailure == "readback");
                                executedCases = std::move(readbacks.cases);
                                for (auto& record : executedCases)
                                    record["execution_status"] = record.at("status");
                                stage(currentStages, "readback_cases");
                                if (!readbacks.passed)
                                    throw Failure("readback_mismatch",
                                                  "Known GPU readback differs from fixed reference "
                                                  "tolerance; inspect case checks/artifacts");
                                lifecycle = exerciseLifecycle(context, *selected, options);
                                stage(currentStages, "lifecycle");
                            } else {
                                glfwShowWindow(context.window);
                                glfwPollEvents();
                                presentFrame(context);
                                ++lifecycle.frames;
                            }
                            if (!context.validation->waitForIdle())
                                throw Failure("device_wait",
                                              "Failed to retire submitted NVRHI work");
                            context.validation->runGarbageCollection();
                            requireVk(vkDeviceWaitIdle(context.device),
                                      "wait before device teardown");
                        }
                    }
                }
                ++completedDevices;
            }
            if (options.mode == "cases") {
                const bool fullLifecycle = lifecycle.resizeCycles >= 100 &&
                                           completedDevices >= 20 && lifecycle.zeroExtents > 0 &&
                                           lifecycle.minimized >= 100 && lifecycle.frames > 0 &&
                                           lifecycle.seconds >= 600;
                report["lifecycle"] = Object{
                    {"requested_resize_cycles", options.lifecycleCycles},
                    {"completed_resize_cycles", lifecycle.resizeCycles},
                    {"requested_device_cycles", options.deviceCycles},
                    {"completed_device_cycles", completedDevices},
                    {"zero_extent_policy_checks", 0U},
                    {"observed_zero_framebuffer_extents", lifecycle.zeroExtents},
                    {"minimize_restore_cycles", lifecycle.minimized},
                    {"presented_frames", lifecycle.frames},
                    {"requested_present_seconds", options.presentSeconds},
                    {"present_elapsed_seconds", lifecycle.seconds},
                    {"fully_qualified", fullLifecycle && options.validation},
                    {"zero_extent_note", "CPU guard tests cover zero dimensions; actual OS zero "
                                         "framebuffer observations are counted separately"},
                    {"presentation_path",
                     "serialized Vulkan color-attachment clear; GPU draw/compute cases use NVRHI"},
                    {"retirement", "submission fences and device/queue idle; no concurrent frames "
                                   "or reusable present semaphores"}};
                executedCases.insert(
                    executedCases.begin(),
                    Object{
                        {"id", "P02-01"},
                        {"name", "stack_device"},
                        {"status", "PASS"},
                        {"execution_status", "PASS"},
                        {"reason",
                         "Explicit capable device, NVRHI, swapchain and presentation completed"}});
                executedCases.push_back(Object{
                    {"id", "P02-05"},
                    {"name", "lifecycle"},
                    {"status", fullLifecycle ? "PASS" : "NOT_RUN"},
                    {"execution_status", "PASS"},
                    {"reason", fullLifecycle
                                   ? "Full lifecycle thresholds observed"
                                   : "Requested bounded lifecycle completed; full 100/20/600s plus "
                                     "observed zero-extent gate remains open"}});
                executedCases.push_back(
                    Object{{"id", "P02-06"},
                           {"name", "validation"},
                           {"status", "PASS"},
                           {"execution_status", options.validation ? "PASS" : "NOT_RUN"},
                           {"reason", "All enabled validation messages, including teardown, "
                                      "checked before success"}});
                for (const auto& id : {"P02-07", "P02-08"})
                    executedCases.push_back(
                        Object{{"id", id},
                               {"status", "NOT_RUN"},
                               {"execution_status", "NOT_RUN"},
                               {"reason", "Packaging or aggregate failure-path evidence is "
                                          "separate from this successful GPU operation"}});
                if (!options.validation) {
                    for (auto& record : executedCases) {
                        record["status"] = "NOT_RUN";
                        record["reason"] =
                            "Diagnostic execution without Vulkan validation; no case qualification";
                    }
                }
            }
            // Includes validation callbacks emitted during reverse-order cleanup.
            stage(stages, "teardown");
            if (diagnostics.errors != 0 || diagnostics.warnings != 0) {
                throw Failure("validation_messages",
                              "Setup produced validation warnings/errors; inspect retained stderr. "
                              "No automatic waiver is applied.");
            }
            report["status"] = "PASS";
        }
    } catch (const Failure& failure) {
        exitCode = failure.exitCode;
        report["status"] = exitCode == 3 ? "NOT_RUN" : "FAIL";
        report["error"] = Object{{"code", failure.code}, {"message", failure.what()}};
        std::fprintf(stderr, "GraphicsProbe: %s: %s\n", failure.code.c_str(), failure.what());
    } catch (const std::exception& failure) {
        exitCode = 1;
        report["status"] = "FAIL";
        report["error"] = Object{{"code", "exception"}, {"message", failure.what()}};
        std::fprintf(stderr, "GraphicsProbe: exception: %s\n", failure.what());
    }
    if (!executedCases.empty()) {
        Array cases;
        for (std::uint32_t index = 1; index <= 8; ++index) {
            const std::string id = "P02-0" + std::to_string(index);
            Object fallback{{"id", id},
                            {"status", "NOT_RUN"},
                            {"execution_status", "NOT_RUN"},
                            {"reason", "Operation did not complete this case"}};
            bool found = false;
            for (auto& record : executedCases) {
                std::ostringstream key;
                record.at("id").write(key);
                if (key.str() == "\"" + id + "\"") {
                    if (exitCode != 0)
                        record["status"] = "NOT_RUN";
                    if (id == "P02-06" && (diagnostics.errors != 0 || diagnostics.warnings != 0)) {
                        record["execution_status"] = "FAIL";
                        record["reason"] = "Validation or teardown emitted warnings/errors; "
                                           "inspect retained stderr";
                    }
                    cases.emplace_back(record);
                    found = true;
                    break;
                }
            }
            if (!found)
                cases.emplace_back(std::move(fallback));
        }
        report["cases"] = std::move(cases);
    }
    report["stages"] = std::move(stages);
    report["validation_warnings"] = diagnostics.warnings.load();
    report["validation_errors"] = diagnostics.errors.load();
    report["exit_code"] = static_cast<std::uint32_t>(exitCode);
    Json(std::move(report)).write(std::cout);
    std::cout << '\n';
    return std::cout.good() ? exitCode : 1;
}
