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
#include <cstdio>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
};

Options parseOptions(int argc, char** argv) {
    Options options;
    bool deviceSeen = false;
    bool validationSeen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "--version" || argument == "--describe" ||
            argument == "--inventory" || argument == "--smoke") {
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
        } else {
            throw Failure("arguments", "Unknown argument: " + std::string(argument), 2);
        }
    }
    if (options.mode.empty()) {
        throw Failure("arguments", "Choose --describe, --inventory, --smoke, --help or --version",
                      2);
    }
    if (options.mode == "smoke") {
        if (!deviceSeen) {
            throw Failure("arguments",
                          "--smoke requires --device <deviceUUID>; run --inventory first", 2);
        }
    } else if (deviceSeen || validationSeen) {
        throw Failure("arguments", "--device and --validation are available only with --smoke", 2);
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
    bool glfwInitialized = false;
    GLFWwindow* window = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    nvrhi::vulkan::DeviceHandle backend;
    nvrhi::DeviceHandle validation;

    Context() = default;
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context() noexcept {
        validation = nullptr;
        backend = nullptr;
        // No command is submitted by 02a, so teardown has no in-flight work.
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
    }
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Shark GraphicsProbe 02a";
    application.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    application.pEngineName = "Shark probe (no renderer)";
    application.apiVersion = VK_API_VERSION_1_3;
    const char* layerName = "VK_LAYER_KHRONOS_validation";
    auto debug = debugInfo(diagnostics);
    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext = validation ? &debug : nullptr;
    instanceInfo.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    instanceInfo.pApplicationInfo = &application;
    instanceInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
    instanceInfo.ppEnabledExtensionNames = enabled.data();
    instanceInfo.enabledLayerCount = validation ? 1U : 0U;
    instanceInfo.ppEnabledLayerNames = validation ? &layerName : nullptr;
    requireVk(vkCreateInstance(&instanceInfo, nullptr, &context.instance), "vkCreateInstance", 3);
    report["vulkan_validation_enabled"] = validation;
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
    context.window = glfwCreateWindow(64, 64, "Shark GraphicsProbe setup", nullptr, nullptr);
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
    report["enabled_features"] = Array{"timelineSemaphore", "synchronization2"};
    report["selected_queue_family"] = queueFamily;
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(context.device, queueFamily, 0, &queue);
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
    if (images.empty()) {
        throw Failure("swapchain_images", "Created swapchain has no images");
    }
    stage(stages, "swapchain");
    report["swapchain"] =
        Object{{"format", static_cast<std::uint32_t>(swapchainInfo.imageFormat)},
               {"color_space", static_cast<std::uint32_t>(swapchainInfo.imageColorSpace)},
               {"extent", Array{extent.width, extent.height}},
               {"image_count", static_cast<std::uint64_t>(images.size())},
               {"present_mode", "FIFO"},
               {"presented_frames", 0U}};
}

Object baseReport() {
    Array cases;
    constexpr std::array names{"stack_device",          "graphics_depth", "compute",
                               "formats_shaders",       "lifecycle",      "validation",
                               "portability_packaging", "failure_paths"};
    for (std::size_t index = 0; index < names.size(); ++index) {
        cases.emplace_back(
            Object{{"id", "P02-0" + std::to_string(index + 1)},
                   {"name", names[index]},
                   {"status", "NOT_RUN"},
                   {"reason", "02a setup is not full case execution or hardware qualification"}});
    }
    return Object{{"schema_version", 1U},
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
    report["description"] = "Maintained 02a graphics setup probe. No rendering, submission, "
                            "readback or game-authoring API exists.";
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
                          "no submitted work"}}};
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
    Object report = baseReport();
    Array stages;
    Diagnostics diagnostics;
    int exitCode = 0;
    try {
        const Options options = parseOptions(argc, argv);
        report["mode"] = options.mode;
        if (options.mode == "help") {
            std::cout
                << "Shark GraphicsProbe " SHARK_PROBE_VERSION " (02a setup)\n"
                   "  --help | --version | --describe\n"
                   "  --inventory\n"
                   "  --smoke --device <32-lowercase-hex-UUID> [--validation required|off]\n"
                   "Discovery/help/version do not initialize graphics. Inventory requires a "
                   "desktop.\n"
                   "Smoke defaults to required Vulkan validation; off is diagnostic only.\n"
                   "Use tools/graphics_probe.py to enforce a process timeout.\n"
                   "No draw, compute, readback or full platform qualification is implemented.\n";
            return std::cout.good() ? 0 : 1;
        }
        if (options.mode == "version") {
            std::cout << "Shark GraphicsProbe " SHARK_PROBE_VERSION "\n";
            return std::cout.good() ? 0 : 1;
        }
        if (options.mode == "describe") {
            describe(report);
        } else {
            {
                Context context;
                const auto instanceExtensions =
                    initialize(context, diagnostics, options.mode == "smoke" && options.validation,
                               report, stages);
                const auto devices = enumerate<VkPhysicalDevice>(
                    [&context](auto* count, auto* values) {
                        return vkEnumeratePhysicalDevices(context.instance, count, values);
                    },
                    "vkEnumeratePhysicalDevices");
                if (devices.empty()) {
                    throw Failure(
                        "device_unavailable",
                        "No Vulkan physical device found; install the selected GPU driver/ICD", 3);
                }
                Array inventory;
                std::optional<DeviceInfo> selected;
                for (const auto handle : devices) {
                    auto device = inspectDevice(handle, context.surface);
                    inventory.emplace_back(device.report);
                    if (device.uuid == options.device) {
                        if (selected) {
                            throw Failure("ambiguous_device",
                                          "Multiple physical devices have the requested UUID; fix "
                                          "duplicate ICD discovery before selecting a device");
                        }
                        selected = std::move(device);
                    }
                }
                report["devices"] = std::move(inventory);
                stage(stages, "inventory");
                if (options.mode == "smoke") {
                    report["selected_device_uuid"] = options.device;
                    if (!selected) {
                        throw Failure("device_unavailable",
                                      "Requested Vulkan deviceUUID is absent; rerun --inventory "
                                      "and select the intended GPU explicitly",
                                      3);
                    }
                    smoke(context, diagnostics, *selected, instanceExtensions, report, stages);
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
    report["stages"] = std::move(stages);
    report["validation_warnings"] = diagnostics.warnings.load();
    report["validation_errors"] = diagnostics.errors.load();
    report["exit_code"] = static_cast<std::uint32_t>(exitCode);
    Json(std::move(report)).write(std::cout);
    std::cout << '\n';
    return std::cout.good() ? exitCode : 1;
}
