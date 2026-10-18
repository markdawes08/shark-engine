#include "graphics_backend.hpp"
#include "graphics_offscreen.hpp"
#include "graphics_queue_waiter.hpp"
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
#include "graphics_native_test.hpp"
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The pinned static NVRHI backend shares this dispatcher and the app-local
// loader. The Window lease prevents overlapping native device lifetimes.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace shark::client {
namespace {

constexpr std::uint32_t enumeration_limit = 4096;

Error failure(ErrorCode code, std::string_view operation, std::string message) {
    return {code, std::move(message), {.operation = std::string(operation)}};
}

// These helpers throw only inside the backend's synchronous implementation.
// Public phases convert owned Error values back into Result before returning.
// Allocation and third-party exceptions reach the owning lifecycle's guard.
void require_vk(VkResult result, std::string_view operation) {
    if (result != VK_SUCCESS) {
        throw failure(ErrorCode::execution_failed, operation,
                      "Vulkan operation failed with VkResult " + std::to_string(result));
    }
}

template <typename T, typename Function> Result<T> phase(Function&& function) {
    try {
        return function();
    } catch (Error& error) {
        return std::move(error);
    }
}

template <typename T, typename Enumerate>
std::vector<T> enumerate(Enumerate&& function, std::string_view operation) {
    std::uint32_t count = 0;
    require_vk(function(&count, static_cast<T*>(nullptr)), operation);
    if (count > enumeration_limit) {
        throw failure(ErrorCode::execution_failed, operation,
                      "Vulkan enumeration exceeds the 4096-entry bound");
    }
    std::vector<T> values(count);
    if (count != 0) {
        const auto expected = count;
        require_vk(function(&count, values.data()), operation);
        if (count != expected) {
            throw failure(ErrorCode::execution_failed, operation,
                          "Vulkan enumeration changed; retry the request explicitly");
        }
    }
    return values;
}

bool contains_extension(const std::vector<VkExtensionProperties>& values, std::string_view name) {
    return std::any_of(values.begin(), values.end(), [name](const auto& value) {
        return std::string_view(value.extensionName) == name;
    });
}

template <std::size_t Size> std::string bounded_text(const char (&value)[Size]) {
    return {value, std::find(value, value + Size, '\0')};
}

std::string device_uuid(const std::uint8_t* bytes) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(VK_UUID_SIZE * 2, '0');
    for (std::size_t index = 0; index < VK_UUID_SIZE; ++index) {
        result[index * 2] = digits[bytes[index] >> 4U];
        result[index * 2 + 1] = digits[bytes[index] & 0xFU];
    }
    return result;
}

AdapterKind adapter_kind(VkPhysicalDeviceType type) noexcept {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return AdapterKind::integrated;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return AdapterKind::discrete;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return AdapterKind::software;
    default:
        return AdapterKind::other;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void* user) noexcept {
    auto& sink = *static_cast<detail::GraphicsDiagnosticSink*>(user);
    sink.record((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0,
                data != nullptr ? data->pMessage : nullptr);
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT debug_info(detail::GraphicsDiagnosticSink& sink) noexcept {
    VkDebugUtilsMessengerCreateInfoEXT result{};
    result.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    result.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    result.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    result.pfnUserCallback = validation_message;
    result.pUserData = &sink;
    return result;
}

class NvrhiDiagnostics final : public nvrhi::IMessageCallback {
  public:
    detail::GraphicsDiagnosticSink* sink{nullptr};

    void message(nvrhi::MessageSeverity severity, const char* text) noexcept override {
        if (sink != nullptr && (severity == nvrhi::MessageSeverity::Warning ||
                                severity == nvrhi::MessageSeverity::Error ||
                                severity == nvrhi::MessageSeverity::Fatal)) {
            sink->record(severity != nvrhi::MessageSeverity::Warning, text);
        }
    }
};

struct FormatRequirement {
    VkFormat format;
    VkFormatFeatureFlags features;
    VkImageUsageFlags usage;
};

// The unchanged assignment-02 combined format/usage profile.
constexpr VkFormatFeatureFlags sampled =
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
constexpr VkFormatFeatureFlags transfer =
    VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
constexpr VkFormatFeatureFlags color =
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
constexpr VkImageUsageFlags upload =
    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr std::array format_requirements{
    FormatRequirement{VK_FORMAT_R8G8B8A8_SRGB, sampled | transfer, upload},
    FormatRequirement{VK_FORMAT_R8G8B8A8_UNORM, sampled | transfer | color,
                      upload | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT},
    FormatRequirement{VK_FORMAT_R16G16B16A16_SFLOAT,
                      sampled | transfer | color | VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                      upload | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT},
    FormatRequirement{VK_FORMAT_D32_SFLOAT,
                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                          VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT}};

GraphicsAdapter inspect_adapter(VkPhysicalDevice handle, VkSurfaceKHR surface) {
    GraphicsAdapter result;
    VkPhysicalDeviceProperties basic{};
    vkGetPhysicalDeviceProperties(handle, &basic);
    // These core 1.1 structures are valid for the loader's 1.3 instance even if
    // a returned device fails the later 1.3 admission check. Driver properties
    // and the 1.2/1.3 feature structs only apply at their device API versions.
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceIDProperties identity{};
    identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    identity.pNext = basic.apiVersion >= VK_API_VERSION_1_2 ? &driver : nullptr;
    VkPhysicalDeviceProperties2 properties{};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &identity;
    vkGetPhysicalDeviceProperties2(handle, &properties);
    result.uuid = device_uuid(identity.deviceUUID);
    result.name = bounded_text(basic.deviceName);
    result.driver_name = bounded_text(driver.driverName);
    result.driver_info = bounded_text(driver.driverInfo);
    result.vendor_id = basic.vendorID;
    result.device_id = basic.deviceID;
    result.driver_version = basic.driverVersion;
    result.api_version = basic.apiVersion;
    result.kind = adapter_kind(basic.deviceType);

    VkPhysicalDeviceVulkan13Features features13{};
    features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.pNext = basic.apiVersion >= VK_API_VERSION_1_3 ? &features13 : nullptr;
    VkPhysicalDeviceFeatures2 features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features.pNext = basic.apiVersion >= VK_API_VERSION_1_2 ? &features12 : nullptr;
    vkGetPhysicalDeviceFeatures2(handle, &features);
    const auto extensions = enumerate<VkExtensionProperties>(
        [handle](auto* count, auto* values) {
            return vkEnumerateDeviceExtensionProperties(handle, nullptr, count, values);
        },
        "graphics.adapter.extensions");
    auto& supported = result.capabilities.features;
    supported[0] = basic.apiVersion >= VK_API_VERSION_1_3;
    supported[1] = features12.timelineSemaphore == VK_TRUE;
    supported[2] = features13.synchronization2 == VK_TRUE;
    supported[3] = features13.dynamicRendering == VK_TRUE;
    supported[4] = contains_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    const auto& limits = basic.limits;
    result.capabilities.limits = {
        limits.maxImageDimension2D,           limits.maxImageDimensionCube,
        limits.maxImageArrayLayers,           limits.maxColorAttachments,
        limits.maxBoundDescriptorSets,        limits.maxPerStageDescriptorSampledImages,
        limits.maxPerStageDescriptorSamplers, limits.maxUniformBufferRange,
        limits.maxStorageBufferRange,         limits.maxComputeWorkGroupInvocations};

    std::uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(handle, &queue_count, nullptr);
    if (queue_count == 0 || queue_count > enumeration_limit) {
        throw failure(ErrorCode::execution_failed, "graphics.adapter.queues",
                      "Vulkan queue-family count is outside 1..4096");
    }
    const auto expected_count = queue_count;
    std::vector<VkQueueFamilyProperties> queues(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(handle, &queue_count, queues.data());
    if (queue_count != expected_count) {
        throw failure(ErrorCode::execution_failed, "graphics.adapter.queues",
                      "Vulkan queue-family enumeration changed; retry explicitly");
    }
    for (std::uint32_t index = 0; index < queue_count; ++index) {
        VkBool32 present = VK_FALSE;
        require_vk(vkGetPhysicalDeviceSurfaceSupportKHR(handle, index, surface, &present),
                   "graphics.adapter.surface_support");
        constexpr VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        const auto& queue = queues[index];
        if (!supported[5] && queue.queueCount != 0 && (queue.queueFlags & required) == required &&
            present == VK_TRUE) {
            supported[5] = true;
            result.queue_family = index;
        }
    }
    for (std::size_t index = 0; index < format_requirements.size(); ++index) {
        const auto& required = format_requirements[index];
        VkFormatProperties format{};
        vkGetPhysicalDeviceFormatProperties(handle, required.format, &format);
        VkImageFormatProperties image{};
        const auto status = vkGetPhysicalDeviceImageFormatProperties(
            handle, required.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, required.usage, 0,
            &image);
        if (status != VK_SUCCESS && status != VK_ERROR_FORMAT_NOT_SUPPORTED) {
            require_vk(status, "graphics.adapter.image_format");
        }
        const bool depth_array = required.format != VK_FORMAT_D32_SFLOAT ||
                                 (image.maxExtent.width >= 2048 && image.maxExtent.height >= 2048 &&
                                  image.maxArrayLayers >= 12);
        result.capabilities.formats[index] =
            status == VK_SUCCESS && depth_array &&
            (format.optimalTilingFeatures & required.features) == required.features;
    }
    const auto formats = enumerate<VkSurfaceFormatKHR>(
        [handle, surface](auto* count, auto* values) {
            return vkGetPhysicalDeviceSurfaceFormatsKHR(handle, surface, count, values);
        },
        "graphics.adapter.surface_formats");
    supported[6] = std::any_of(formats.begin(), formats.end(), [](const auto& format) {
        return (format.format == VK_FORMAT_B8G8R8A8_SRGB ||
                format.format == VK_FORMAT_R8G8B8A8_SRGB || format.format == VK_FORMAT_UNDEFINED) &&
               format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    });
    const auto modes = enumerate<VkPresentModeKHR>(
        [handle, surface](auto* count, auto* values) {
            return vkGetPhysicalDeviceSurfacePresentModesKHR(handle, surface, count, values);
        },
        "graphics.adapter.present_modes");
    supported[7] = std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) != modes.end();
    return result;
}

class VulkanBackend final : public detail::GraphicsBackend {
  public:
    VulkanBackend() = default;
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
    explicit VulkanBackend(detail::NativeGraphicsTestControl& test) noexcept : test_(&test) {}
#endif
    ~VulkanBackend() noexcept override {
        if (queue_waiter_.pending() || offscreen_) {
            std::terminate();
        }
    }
    Result<void> create_window(const WindowConfig& config) override {
        auto created = Window::create(config);
        if (!created) {
            return std::move(created).error();
        }
        window_ = std::move(created).value();
        return {};
    }

    Result<void> create_instance(GraphicsValidation validation,
                                 detail::GraphicsDiagnosticSink& sink) override {
        return phase<void>([&]() -> Result<void> {
            std::uint32_t version = VK_API_VERSION_1_0;
            const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
                vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
            if (enumerate_version != nullptr) {
                require_vk(enumerate_version(&version), "graphics.loader.version");
            }
            if (version < VK_API_VERSION_1_3) {
                return failure(ErrorCode::capability_unavailable, "graphics.loader.version",
                               "A Vulkan 1.3 loader is required");
            }
            const auto extensions = enumerate<VkExtensionProperties>(
                [](auto* count, auto* values) {
                    return vkEnumerateInstanceExtensionProperties(nullptr, count, values);
                },
                "graphics.instance.extensions");
            instance_extensions_ = {VK_KHR_SURFACE_EXTENSION_NAME,
                                    VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
            for (const char* required : instance_extensions_) {
                if (!contains_extension(extensions, required)) {
                    return failure(ErrorCode::capability_unavailable,
                                   "graphics.instance.extensions",
                                   "Missing required instance extension: " + std::string(required));
                }
            }
            // Optional present fences are used only when both the instance
            // dependencies and selected device feature are available. The
            // current Intel driver instead uses host-completed submissions.
            surface_maintenance_ =
                contains_extension(extensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
                contains_extension(extensions, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            if (surface_maintenance_) {
                instance_extensions_.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
                instance_extensions_.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
            }
            const bool validate = validation == GraphicsValidation::required;
            constexpr const char* layer_name = "VK_LAYER_KHRONOS_validation";
            if (validate) {
                const auto layers = enumerate<VkLayerProperties>(
                    [](auto* count, auto* values) {
                        return vkEnumerateInstanceLayerProperties(count, values);
                    },
                    "graphics.instance.layers");
                const bool available =
                    std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
                        return std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
                    });
                if (!available ||
                    !contains_extension(extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
                    return failure(ErrorCode::capability_unavailable,
                                   "graphics.instance.validation",
                                   "Required VK_LAYER_KHRONOS_validation or VK_EXT_debug_utils is "
                                   "unavailable; "
                                   "prepare the pinned validation tools and set VK_LAYER_PATH");
                }
                const auto layer_extensions = enumerate<VkExtensionProperties>(
                    [](auto* count, auto* values) {
                        return vkEnumerateInstanceExtensionProperties("VK_LAYER_KHRONOS_validation",
                                                                      count, values);
                    },
                    "graphics.instance.validation_extensions");
                if (!contains_extension(layer_extensions,
                                        VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)) {
                    return failure(ErrorCode::capability_unavailable,
                                   "graphics.instance.validation",
                                   "Required VK_EXT_validation_features synchronization validation "
                                   "is unavailable");
                }
                instance_extensions_.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                instance_extensions_.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            }
            VkApplicationInfo application{};
            application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
            application.pApplicationName = "Shark";
            application.pEngineName = "Shark";
            application.apiVersion = VK_API_VERSION_1_3;
            auto debug = debug_info(sink);
            constexpr VkValidationFeatureEnableEXT sync_validation =
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
            VkValidationFeaturesEXT validation_features{};
            validation_features.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
            validation_features.pNext = &debug;
            validation_features.enabledValidationFeatureCount = 1;
            validation_features.pEnabledValidationFeatures = &sync_validation;
            VkInstanceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
            info.pNext = validate ? &validation_features : nullptr;
            info.pApplicationInfo = &application;
            info.enabledExtensionCount = static_cast<std::uint32_t>(instance_extensions_.size());
            info.ppEnabledExtensionNames = instance_extensions_.data();
            info.enabledLayerCount = validate ? 1U : 0U;
            info.ppEnabledLayerNames = validate ? &layer_name : nullptr;
            require_vk(vkCreateInstance(&info, nullptr, &instance_), "graphics.instance.create");
            if (validate) {
                const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
                destroy_messenger_ = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
                if (create == nullptr || destroy_messenger_ == nullptr) {
                    return failure(
                        ErrorCode::execution_failed, "graphics.instance.validation",
                        "Required debug-utils create/destroy entry points are unavailable");
                }
                require_vk(create(instance_, &debug, nullptr, &messenger_),
                           "graphics.instance.debug_messenger");
            }
            return {};
        });
    }

    Result<void> create_surface() override {
        return phase<void>([&]() -> Result<void> {
            auto native = detail::native_test_handle(*window_);
            if (!native) {
                return std::move(native).error();
            }
            const auto hwnd = static_cast<HWND>(native.value());
            const auto hinstance =
                reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            if (hwnd == nullptr || hinstance == nullptr) {
                return failure(ErrorCode::execution_failed, "graphics.surface.create",
                               "Owned window has no valid Win32 HWND/HINSTANCE");
            }
            VkWin32SurfaceCreateInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
            info.hinstance = hinstance;
            info.hwnd = hwnd;
            require_vk(vkCreateWin32SurfaceKHR(instance_, &info, nullptr, &surface_),
                       "graphics.surface.create");
            return {};
        });
    }

    Result<std::vector<GraphicsAdapter>> enumerate_adapters() override {
        return phase<std::vector<GraphicsAdapter>>([&]() -> Result<std::vector<GraphicsAdapter>> {
            physical_devices_ = enumerate<VkPhysicalDevice>(
                [&](auto* count, auto* values) {
                    return vkEnumeratePhysicalDevices(instance_, count, values);
                },
                "graphics.adapters.enumerate");
            // Bound expensive per-adapter queries too, before the generic
            // owner validates metadata and duplicate identities.
            if (physical_devices_.size() > 64) {
                return failure(ErrorCode::execution_failed, "graphics.adapters.enumerate",
                               "More than 64 graphics adapters were reported");
            }
            adapters_.clear();
            adapters_.reserve(physical_devices_.size());
            for (auto handle : physical_devices_) {
                adapters_.push_back(inspect_adapter(handle, surface_));
            }
            return adapters_;
        });
    }

    Result<void> create_device(std::size_t adapter_index) override {
        return phase<void>([&]() -> Result<void> {
            if (adapter_index >= adapters_.size()) {
                return failure(ErrorCode::invalid_argument, "graphics.device.create",
                               "Selected adapter index is out of range");
            }
            selected_ = physical_devices_[adapter_index];
            queue_family_ = adapters_[adapter_index].queue_family;
            const auto extensions = enumerate<VkExtensionProperties>(
                [&](auto* count, auto* values) {
                    return vkEnumerateDeviceExtensionProperties(selected_, nullptr, count, values);
                },
                "graphics.device.extensions");
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{};
            maintenance.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT;
            if (surface_maintenance_ &&
                contains_extension(extensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
                VkPhysicalDeviceFeatures2 features{};
                features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
                features.pNext = &maintenance;
                vkGetPhysicalDeviceFeatures2(selected_, &features);
            }
            present_fences_ = maintenance.swapchainMaintenance1 == VK_TRUE;
            const float priority = 1.0F;
            VkDeviceQueueCreateInfo queue_info{};
            queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queue_info.queueFamilyIndex = queue_family_;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;
            VkPhysicalDeviceVulkan13Features enabled13{};
            enabled13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            enabled13.synchronization2 = VK_TRUE;
            enabled13.dynamicRendering = VK_TRUE;
            enabled13.pNext = present_fences_ ? &maintenance : nullptr;
            VkPhysicalDeviceVulkan12Features enabled12{};
            enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            enabled12.pNext = &enabled13;
            enabled12.timelineSemaphore = VK_TRUE;
            device_extensions_ = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            if (present_fences_) {
                device_extensions_.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            }
            VkDeviceCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            info.pNext = &enabled12;
            info.queueCreateInfoCount = 1;
            info.pQueueCreateInfos = &queue_info;
            info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions_.size());
            info.ppEnabledExtensionNames = device_extensions_.data();
            require_vk(vkCreateDevice(selected_, &info, nullptr, &device_),
                       "graphics.device.create");
            vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
            if (queue_ == VK_NULL_HANDLE) {
                return failure(ErrorCode::execution_failed, "graphics.device.queue",
                               "Vulkan returned a null graphics queue");
            }
            return {};
        });
    }

    Result<void> create_nvrhi(detail::GraphicsDiagnosticSink& sink) override {
        return phase<void>([&]() -> Result<void> {
            nvrhi_diagnostics_.sink = &sink;
            VULKAN_HPP_DEFAULT_DISPATCHER.init(instance_, vkGetInstanceProcAddr, device_);
            nvrhi::vulkan::DeviceDesc info{};
            info.errorCB = &nvrhi_diagnostics_;
            info.instance = instance_;
            info.physicalDevice = selected_;
            info.device = device_;
            info.graphicsQueue = queue_;
            info.graphicsQueueIndex = static_cast<int>(queue_family_);
            info.instanceExtensions = instance_extensions_.data();
            info.numInstanceExtensions = instance_extensions_.size();
            info.deviceExtensions = device_extensions_.data();
            info.numDeviceExtensions = device_extensions_.size();
            nvrhi_ = nvrhi::vulkan::createDevice(info);
            if (!nvrhi_) {
                return failure(ErrorCode::execution_failed, "graphics.nvrhi.create",
                               "NVRHI Vulkan device creation returned null");
            }
            validation_ = nvrhi::validation::createValidationLayer(nvrhi_);
            if (!validation_) {
                return failure(ErrorCode::execution_failed, "graphics.nvrhi.validation",
                               "NVRHI validation wrapper creation returned null");
            }
            return {};
        });
    }

    Result<GraphicsWindowFrame> poll_window() override {
        if (auto polled = window_->poll_events(); !polled) {
            return polled.error();
        }
        auto state = window_->state();
        if (!state) {
            return state.error();
        }
        auto input = window_->capture_input();
        if (!input) {
            return input.error();
        }
        auto diagnostics = window_->take_diagnostics();
        if (!diagnostics) {
            return diagnostics.error();
        }
        return GraphicsWindowFrame{state.value(), std::move(input).value(),
                                   std::move(diagnostics).value()};
    }

    Result<WindowState> window_state() override {
        return window_->state();
    }

    Result<void> window_command(detail::WindowCommand command, WindowExtent extent) override {
        switch (command) {
        case detail::WindowCommand::resize:
            return window_->resize(extent);
        case detail::WindowCommand::iconify:
            return window_->iconify();
        case detail::WindowCommand::restore:
            return window_->restore();
        case detail::WindowCommand::show:
            return window_->show();
        case detail::WindowCommand::focus:
            return window_->focus();
        case detail::WindowCommand::request_close:
            return window_->request_close();
        }
        return failure(ErrorCode::invalid_argument, "graphics.window.command",
                       "Unknown window command");
    }

    Result<void> set_presentation_draw(const OffscreenRequest& request,
                                       bool enable_readback) override {
        if (window_draw_ || swapchain_ != VK_NULL_HANDLE) {
            return failure(ErrorCode::conflict, "graphics.window_draw.configuration",
                           "Window draw is immutable after configuration");
        }
        window_draw_ = request;
        capture_enabled_ = enable_readback;
        return {};
    }

    Result<OffscreenReadback> capture_presented() override {
        return phase<OffscreenReadback>([&]() -> Result<OffscreenReadback> {
            if (!capture_enabled_ || !captured_image_ || device_lost_) {
                return failure(ErrorCode::conflict, "graphics.capture.state",
                               "There is no completed presentation candidate to capture");
            }
            const auto& image = images_.at(*captured_image_);
            if (!image.draw || image.draw->submitted == 0) {
                return failure(ErrorCode::conflict, "graphics.capture.state",
                               "Presented draw has no capture submission");
            }
            bool force_timeout = false;
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
            force_timeout = inject(detail::NativeGraphicsFault::window_capture_timeout);
#endif
            if (force_timeout || !wait_timeline(image.draw->submitted, 1'000'000'000ULL)) {
                return failure(ErrorCode::execution_failed, "graphics.capture.wait",
                               "Capture wait timed out; all presentation owners remain live");
            }
            auto result = detail::read_offscreen_resources(*validation_, *image.draw);
            result.color_encoding = ReadbackColorEncoding::srgb;
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(selected_, &properties);
            result.subpixel_precision_bits = properties.limits.subPixelPrecisionBits;
            if (image.texture->getDesc().format == nvrhi::Format::SBGRA8_UNORM) {
                for (std::size_t index = 0; index < result.rgba8.size(); index += 4) {
                    std::swap(result.rgba8[index], result.rgba8[index + 2]);
                }
            }
            // Readback does not retire reusable per-image owners.
            return result;
        });
    }

    Result<PresentationSync> create_presentation() override {
        return phase<PresentationSync>([&]() -> Result<PresentationSync> {
            for (auto& frame : frames_) {
                VkSemaphoreCreateInfo semaphore{};
                semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                check(vkCreateSemaphore(device_, &semaphore, nullptr, &frame.acquired),
                      "graphics.presentation.acquire_semaphore");
                VkFenceCreateInfo fence{};
                fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                check(vkCreateFence(device_, &fence, nullptr, &frame.acquire_fence),
                      "graphics.presentation.acquire_fence");
                frame.commands = validation_->createCommandList();
                if (!frame.commands) {
                    return failure(ErrorCode::execution_failed, "graphics.presentation.commands",
                                   "NVRHI command list creation returned null");
                }
            }
            return present_fences_ ? PresentationSync::present_fence
                                   : PresentationSync::host_completion;
        });
    }

    Result<std::optional<detail::SwapchainInfo>> create_swapchain(WindowExtent requested) override {
        return phase<std::optional<detail::SwapchainInfo>>(
            [&]() -> Result<std::optional<detail::SwapchainInfo>> {
                VkSurfaceCapabilitiesKHR capabilities{};
                const auto status = observe(
                    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(selected_, surface_, &capabilities));
                if (status == VK_ERROR_OUT_OF_DATE_KHR) {
                    return std::optional<detail::SwapchainInfo>{};
                }
                check(status, "graphics.swapchain.capabilities");
                auto extent = capabilities.currentExtent;
                if (extent.width == std::numeric_limits<std::uint32_t>::max()) {
                    extent = {std::clamp(requested.width, capabilities.minImageExtent.width,
                                         capabilities.maxImageExtent.width),
                              std::clamp(requested.height, capabilities.minImageExtent.height,
                                         capabilities.maxImageExtent.height)};
                }
                if (extent.width == 0 || extent.height == 0) {
                    return std::optional<detail::SwapchainInfo>{};
                }
                if (window_draw_ && (extent.width > 4096 || extent.height > 4096)) {
                    return failure(ErrorCode::capability_unavailable, "graphics.window_draw.extent",
                                   "Diagnostic window draw extent exceeds 4096 per dimension");
                }
                if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
                    return failure(ErrorCode::capability_unavailable, "graphics.swapchain.usage",
                                   "Surface does not support color attachment images");
                }
                if (capture_enabled_ &&
                    (capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) == 0) {
                    return failure(ErrorCode::capability_unavailable, "graphics.capture.usage",
                                   "Surface cannot copy presentation images for explicit capture");
                }
                const auto formats = enumerate<VkSurfaceFormatKHR>(
                    [&](auto* count, auto* values) {
                        return vkGetPhysicalDeviceSurfaceFormatsKHR(selected_, surface_, count,
                                                                    values);
                    },
                    "graphics.swapchain.formats");
                const auto format = std::find_if(formats.begin(), formats.end(), [](const auto& f) {
                    return (f.format == VK_FORMAT_B8G8R8A8_SRGB ||
                            f.format == VK_FORMAT_R8G8B8A8_SRGB ||
                            f.format == VK_FORMAT_UNDEFINED) &&
                           f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
                });
                if (format == formats.end()) {
                    return failure(ErrorCode::capability_unavailable, "graphics.swapchain.format",
                                   "Surface no longer supports an sRGB presentation format");
                }
                constexpr std::array alpha_modes{
                    VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
                const auto alpha =
                    std::find_if(alpha_modes.begin(), alpha_modes.end(), [&](auto candidate) {
                        return (capabilities.supportedCompositeAlpha & candidate) != 0;
                    });
                if (alpha == alpha_modes.end() || capabilities.minImageCount > 8) {
                    return failure(
                        ErrorCode::capability_unavailable, "graphics.swapchain.bounds",
                        "Surface composite alpha or image count exceeds supported bounds");
                }
                auto count = std::min(capabilities.minImageCount + 1, 8U);
                if (capabilities.maxImageCount != 0) {
                    count = std::min(count, capabilities.maxImageCount);
                }
                VkSwapchainCreateInfoKHR info{};
                info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
                info.surface = surface_;
                info.minImageCount = count;
                info.imageFormat = format->format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_SRGB
                                                                         : format->format;
                info.imageColorSpace = format->colorSpace;
                info.imageExtent = extent;
                info.imageArrayLayers = 1;
                info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
                if (capture_enabled_) {
                    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                }
                info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                info.preTransform = capabilities.currentTransform;
                info.compositeAlpha = *alpha;
                info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
                info.clipped = VK_TRUE;
                const auto created =
                    observe(vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_));
                if (created == VK_ERROR_OUT_OF_DATE_KHR) {
                    return std::optional<detail::SwapchainInfo>{};
                }
                check(created, "graphics.swapchain.create");
                const auto handles = enumerate<VkImage>(
                    [&](auto* image_count, auto* values) {
                        return vkGetSwapchainImagesKHR(device_, swapchain_, image_count, values);
                    },
                    "graphics.swapchain.images");
                if (handles.empty() || handles.size() > 8) {
                    return failure(ErrorCode::capability_unavailable, "graphics.swapchain.images",
                                   "Swapchain image count must be between one and eight");
                }
                extent_ = extent;
                images_.resize(handles.size());
                for (std::size_t index = 0; index < handles.size(); ++index) {
                    auto& image = images_[index];
                    VkImageViewCreateInfo view{};
                    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    view.image = handles[index];
                    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    view.format = info.imageFormat;
                    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    check(vkCreateImageView(device_, &view, nullptr, &image.view),
                          "graphics.swapchain.image_view");
                    nvrhi::TextureDesc description;
                    description.width = extent.width;
                    description.height = extent.height;
                    description.format = info.imageFormat == VK_FORMAT_B8G8R8A8_SRGB
                                             ? nvrhi::Format::SBGRA8_UNORM
                                             : nvrhi::Format::SRGBA8_UNORM;
                    description.isRenderTarget = true;
                    description.isShaderResource = false;
                    description.debugName = "Shark swapchain image";
                    image.texture = validation_->createHandleForNativeTexture(
                        nvrhi::ObjectTypes::VK_Image, nvrhi::Object(handles[index]), description);
                    if (!image.texture) {
                        return failure(ErrorCode::execution_failed, "graphics.swapchain.texture",
                                       "NVRHI swapchain image wrapper creation returned null");
                    }
                    if (present_fences_) {
                        VkSemaphoreCreateInfo semaphore{};
                        semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
                        check(vkCreateSemaphore(device_, &semaphore, nullptr, &image.rendered),
                              "graphics.swapchain.present_semaphore");
                        VkFenceCreateInfo fence{};
                        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                        check(vkCreateFence(device_, &fence, nullptr, &image.present_fence),
                              "graphics.swapchain.present_fence");
                    }
                }
                return std::optional{detail::SwapchainInfo{
                    {extent.width, extent.height}, static_cast<std::uint32_t>(images_.size())}};
            });
    }

    Result<bool> wait_slot(std::uint32_t slot, std::uint64_t timeout_ns) override {
        return phase<bool>([&]() -> Result<bool> {
            auto& frame = frames_.at(slot);
            const auto deadline = make_deadline(timeout_ns);
            if (!wait_queue_idle(remaining(deadline), false)) {
                return false;
            }
            if (!wait_timeline(frame.submitted, remaining(deadline))) {
                return false;
            }
            if (frame.acquire_pending && !wait_fence(frame.acquire_fence, remaining(deadline))) {
                return false;
            }
            frame.acquire_pending = false;
            frame.submitted = 0;
            validation_->runGarbageCollection();
            return true;
        });
    }

    Result<detail::AcquiredImage> acquire(std::uint32_t slot, std::uint64_t timeout_ns) override {
        return phase<detail::AcquiredImage>([&]() -> Result<detail::AcquiredImage> {
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
            if (inject(detail::NativeGraphicsFault::surface_error)) {
                return failure(
                    ErrorCode::execution_failed, "graphics.test.surface_error",
                    "Injected surface-loss policy error; the native driver remains healthy");
            }
            if (inject(detail::NativeGraphicsFault::device_error)) {
                // Deliberately do not set device_lost_: cleanup must still prove
                // actual healthy-driver completion, never fake its loss shortcut.
                return failure(
                    ErrorCode::execution_failed, "graphics.test.device_error",
                    "Injected device-loss policy error; the native driver remains healthy");
            }
#endif
            auto& frame = frames_.at(slot);
            check(vkResetFences(device_, 1, &frame.acquire_fence), "graphics.acquire.reset_fence");
            std::uint32_t image = 0;
            const auto status = observe(vkAcquireNextImageKHR(
                device_, swapchain_, timeout_ns, frame.acquired, frame.acquire_fence, &image));
            if (status == VK_TIMEOUT || status == VK_NOT_READY) {
                return detail::AcquiredImage{detail::AcquireStatus::waiting};
            }
            if (status == VK_ERROR_OUT_OF_DATE_KHR) {
                return detail::AcquiredImage{detail::AcquireStatus::out_of_date};
            }
            if (status != VK_SUBOPTIMAL_KHR) {
                check(status, "graphics.acquire");
            }
            frame.acquire_pending = true;
            if (image >= images_.size()) {
                return failure(ErrorCode::execution_failed, "graphics.acquire.image",
                               "Vulkan returned an invalid swapchain image index");
            }
            return detail::AcquiredImage{status == VK_SUBOPTIMAL_KHR
                                             ? detail::AcquireStatus::suboptimal
                                             : detail::AcquireStatus::ready,
                                         image};
        });
    }

    Result<void> submit(std::uint32_t slot, std::uint32_t index, ClearColor clear) override {
        return phase<void>([&]() -> Result<void> {
            if (queue_waiter_.pending()) {
                return failure(ErrorCode::conflict, "graphics.submit",
                               "The owned queue retirement wait has not completed");
            }
            auto& frame = frames_.at(slot);
            auto& image = images_.at(index);
            // Acquiring an image makes its previous presentation complete. The
            // explicit present fence also proves ownership of the semaphore
            // payload before reuse, independently of the frame-slot fence.
            if (image.present_pending) {
                if (!wait_fence(image.present_fence, 1'000'000'000ULL)) {
                    return failure(ErrorCode::execution_failed, "graphics.submit.present_fence",
                                   "Previous presentation did not retire within one second");
                }
                image.present_pending = false;
            }
            if (window_draw_ && !image.draw) {
                bool fail_partial = false;
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
                fail_partial = inject(detail::NativeGraphicsFault::window_draw_partial_creation);
#endif
                image.draw = detail::create_window_draw_resources(
                    *validation_, *window_draw_, image.texture, {extent_.width, extent_.height},
                    capture_enabled_, fail_partial);
                if (nvrhi_diagnostics_.sink->has_errors()) {
                    image.draw.reset();
                    return failure(ErrorCode::execution_failed, "graphics.window_draw.create",
                                   "Validation reported an error while creating draw resources");
                }
            }
            captured_image_.reset();
            auto& commands = frame.commands;
            // NVRHI retains a self-reference from the opened command buffer
            // until successful execution. Its public API has no discard path,
            // so failed recording must retain ownership even after device loss.
            frame.unretired_recording = true;
            commands->open();
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
            if (inject(detail::NativeGraphicsFault::recording_error)) {
                return failure(ErrorCode::execution_failed, "graphics.test.recording_error",
                               "Injected failure after opening a real NVRHI command list");
            }
            if (inject(detail::NativeGraphicsFault::recording_exception)) {
                throw std::runtime_error(
                    "Injected exception after opening a real NVRHI command list");
            }
#endif
            if (nvrhi_diagnostics_.sink->has_errors()) {
                return failure(ErrorCode::execution_failed, "graphics.submit.validation",
                               "Validation reported an error while opening presentation commands");
            }
            commands->beginTrackingTextureState(image.texture, nvrhi::AllSubresources,
                                                image.initialized ? nvrhi::ResourceStates::Present
                                                                  : nvrhi::ResourceStates::Common);
            commands->setTextureState(image.texture, nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::RenderTarget);
            commands->commitBarriers();
            if (nvrhi_diagnostics_.sink->has_errors()) {
                return failure(
                    ErrorCode::execution_failed, "graphics.submit.validation",
                    "Validation reported an error while preparing presentation commands");
            }
            // Dynamic-rendering loadOp CLEAR needs only the existing required
            // COLOR_ATTACHMENT usage. NVRHI clearTextureFloat would impose an
            // additional TRANSFER_DST surface requirement.
            const auto native = static_cast<VkCommandBuffer>(
                commands->getNativeObject(nvrhi::ObjectTypes::VK_CommandBuffer));
            if (native == VK_NULL_HANDLE) {
                return failure(ErrorCode::execution_failed, "graphics.submit.command_buffer",
                               "NVRHI did not provide a native presentation command buffer");
            }
            VkRenderingAttachmentInfo attachment{};
            attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            attachment.imageView = image.view;
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.clearValue.color =
                window_draw_ ? VkClearColorValue{{0.0F, 0.0F, 0.0F, 1.0F}}
                             : VkClearColorValue{{clear.red, clear.green, clear.blue, clear.alpha}};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea.extent = extent_;
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &attachment;
            vkCmdBeginRendering(native, &rendering);
            vkCmdEndRendering(native);
            if (image.draw) {
                // The native loadOp clear writes the same attachment that the
                // shared NVRHI draw subsequently loads. Preserve that explicit
                // write/load dependency without requiring TRANSFER_DST usage.
                VkMemoryBarrier2 barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                barrier.dstAccessMask =
                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                VkDependencyInfo dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
                dependency.memoryBarrierCount = 1;
                dependency.pMemoryBarriers = &barrier;
                vkCmdPipelineBarrier2(native, &dependency);
                detail::record_window_draw_resources(*image.draw, *window_draw_, *commands,
                                                     !image.uploaded);
            }
            commands->setTextureState(image.texture, nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::Present);
            commands->commitBarriers();
            commands->close();
            if (nvrhi_diagnostics_.sink->has_errors()) {
                return failure(
                    ErrorCode::execution_failed, "graphics.submit.validation",
                    "Validation reported an error while recording presentation commands");
            }
            nvrhi_->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, frame.acquired, 0);
            if (present_fences_) {
                nvrhi_->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, image.rendered, 0);
            }
            // A third-party exception can occur after vkQueueSubmit but before
            // NVRHI returns its serial. Never treat that ambiguous state as no
            // work: retain ownership until process exit rather than free live
            // command pools or swapchain views.
            try {
                frame.submitted = validation_->executeCommandList(commands);
            } catch (const vk::DeviceLostError&) {
                device_lost_ = true;
                return failure(ErrorCode::execution_failed, "graphics.submit.device_lost",
                               "Vulkan device was lost during submission");
            }
            if (frame.submitted == 0) {
                return failure(ErrorCode::execution_failed, "graphics.submit",
                               "NVRHI did not return a valid submission serial");
            }
            frame.unretired_recording = false;
            if (nvrhi_diagnostics_.sink->has_errors()) {
                // The pinned NVRHI queue catches DeviceLostError and reports
                // it through the callback. Stop before presenting a possibly
                // unsignaled semaphore; the finite drain observes device loss.
                return failure(ErrorCode::execution_failed, "graphics.submit.validation",
                               "Validation reported an error during command submission");
            }
            image.initialized = true;
            if (image.draw) {
                image.uploaded = true;
                image.draw->submitted = frame.submitted;
            }
            return {};
        });
    }

    Result<detail::PresentStatus> present(std::uint32_t slot, std::uint32_t index,
                                          std::uint64_t timeout_ns) override {
        return phase<detail::PresentStatus>([&]() -> Result<detail::PresentStatus> {
            auto& image = images_.at(index);
            const auto deadline = make_deadline(timeout_ns);
            if (!wait_queue_idle(remaining(deadline), false)) {
                return detail::PresentStatus::waiting;
            }
            if (!present_fences_ &&
                !wait_timeline(frames_.at(slot).submitted, remaining(deadline))) {
                return detail::PresentStatus::waiting;
            }
            VkSwapchainPresentFenceInfoEXT fence{};
            fence.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT;
            fence.swapchainCount = 1;
            fence.pFences = &image.present_fence;
            if (present_fences_) {
                check(vkResetFences(device_, 1, &image.present_fence),
                      "graphics.present.reset_fence");
            }
            VkPresentInfoKHR info{};
            info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            info.pNext = present_fences_ ? &fence : nullptr;
            info.waitSemaphoreCount = present_fences_ ? 1U : 0U;
            info.pWaitSemaphores = present_fences_ ? &image.rendered : nullptr;
            info.swapchainCount = 1;
            info.pSwapchains = &swapchain_;
            info.pImageIndices = &index;
            const auto status = observe(vkQueuePresentKHR(queue_, &info));
            if (!present_fences_ && status != VK_ERROR_OUT_OF_HOST_MEMORY &&
                status != VK_ERROR_OUT_OF_DEVICE_MEMORY && status != VK_ERROR_DEVICE_LOST) {
                legacy_present_pending_ = true;
            }
            // These WSI failures still enqueue their queue operations. Memory
            // exhaustion leaves the synchronization primitives unaffected.
            image.present_pending = present_fences_ && status != VK_ERROR_OUT_OF_HOST_MEMORY &&
                                    status != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                                    status != VK_ERROR_DEVICE_LOST;
            if (status == VK_ERROR_OUT_OF_DATE_KHR) {
                return detail::PresentStatus::out_of_date;
            }
            if (status == VK_SUBOPTIMAL_KHR) {
                captured_image_ = capture_enabled_ ? std::optional{index} : std::nullopt;
                return detail::PresentStatus::suboptimal;
            }
            check(status, "graphics.present");
            captured_image_ = capture_enabled_ ? std::optional{index} : std::nullopt;
            return detail::PresentStatus::presented;
        });
    }

    bool offscreen_pending() const noexcept override {
        return static_cast<bool>(offscreen_);
    }

    Result<OffscreenReadback> render_offscreen(const OffscreenRequest& request) override {
        return phase<OffscreenReadback>([&]() -> Result<OffscreenReadback> {
            if (offscreen_ || queue_waiter_.pending() || device_lost_) {
                return failure(ErrorCode::conflict, "graphics.offscreen.pending",
                               "GPU owners are pending retirement or the device is lost; drain "
                               "before retrying");
            }
            for (const auto& frame : frames_) {
                if (frame.unretired_recording) {
                    return failure(ErrorCode::conflict, "graphics.offscreen.recording",
                                   "An unresolved presentation recording retains the device");
                }
            }
            bool fail_partial = false;
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
            fail_partial = inject(detail::NativeGraphicsFault::offscreen_partial_creation);
#endif
            offscreen_ = detail::create_offscreen_resources(*validation_, request, fail_partial);
            if (nvrhi_diagnostics_.sink->has_errors()) {
                // No command list has been opened: partial creation may unwind.
                offscreen_.reset();
                return failure(ErrorCode::execution_failed, "graphics.offscreen.create",
                               "Validation reported an error while creating offscreen resources");
            }
            auto& resources = *offscreen_;
            resources.unretired_recording = true;
            resources.commands->open();
            if (nvrhi_diagnostics_.sink->has_errors()) {
                return failure(ErrorCode::execution_failed, "graphics.offscreen.record",
                               "Validation reported an error while opening offscreen commands");
            }
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
            if (inject(detail::NativeGraphicsFault::offscreen_recording_error)) {
                return failure(ErrorCode::execution_failed,
                               "graphics.test.offscreen_recording_error",
                               "Injected failure after opening an owned offscreen command list");
            }
#endif
            detail::record_offscreen_resources(resources, request);
            if (nvrhi_diagnostics_.sink->has_errors()) {
                return failure(ErrorCode::execution_failed, "graphics.offscreen.record",
                               "Validation reported an error while recording offscreen commands");
            }
            try {
                resources.submitted = validation_->executeCommandList(resources.commands);
            } catch (const vk::DeviceLostError&) {
                device_lost_ = true;
                return failure(ErrorCode::execution_failed, "graphics.offscreen.device_lost",
                               "Vulkan device was lost during offscreen submission");
            }
            if (resources.submitted == 0) {
                return failure(ErrorCode::execution_failed, "graphics.offscreen.submit",
                               "NVRHI did not return a valid offscreen submission serial");
            }
            resources.unretired_recording = false;
            if (nvrhi_diagnostics_.sink->has_errors()) {
                return failure(ErrorCode::execution_failed, "graphics.offscreen.submit",
                               "Validation reported an error during offscreen submission");
            }
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
            if (inject(detail::NativeGraphicsFault::offscreen_submission_timeout)) {
                return failure(
                    ErrorCode::execution_failed, "graphics.offscreen.wait_timeout",
                    "Injected unsatisfied completion observation; submitted owners are retained");
            }
#endif
            if (!wait_timeline(resources.submitted, 1'000'000'000ULL)) {
                return failure(
                    ErrorCode::execution_failed, "graphics.offscreen.wait_timeout",
                    "Offscreen submission did not complete within one second; owners are retained");
            }
            validation_->runGarbageCollection();
            auto result = detail::read_offscreen_resources(*validation_, resources);
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(selected_, &properties);
            result.subpixel_precision_bits = properties.limits.subPixelPrecisionBits;
            offscreen_.reset();
            result.retired_resources = result.created_resources;
            return result;
        });
    }

    Result<detail::DrainStatus> drain_presentation(std::uint64_t timeout_ns) override {
        if (offscreen_ && offscreen_->unretired_recording) {
            return failure(
                ErrorCode::execution_failed, "graphics.offscreen.drain",
                "Offscreen recording or submission is unresolved; live resources are retained");
        }
        for (const auto& frame : frames_) {
            if (frame.unretired_recording) {
                return failure(
                    ErrorCode::execution_failed, "graphics.presentation.drain",
                    "Recording or submission ownership is unresolved; live resources are retained");
            }
        }
        try {
            const auto deadline = make_deadline(timeout_ns);
            // Even device-loss teardown cannot destroy the queue while its
            // owned legacy wait thread is still inside the driver.
            if (!wait_queue_idle(remaining(deadline), false)) {
                return detail::DrainStatus::waiting;
            }
            if (device_lost_) {
                offscreen_.reset();
                return detail::DrainStatus::device_lost;
            }
            if (offscreen_ && !wait_timeline(offscreen_->submitted, remaining(deadline))) {
                return detail::DrainStatus::waiting;
            }
            for (auto& frame : frames_) {
                if (!wait_timeline(frame.submitted, remaining(deadline))) {
                    return detail::DrainStatus::waiting;
                }
                if (frame.acquire_pending &&
                    !wait_fence(frame.acquire_fence, remaining(deadline))) {
                    return detail::DrainStatus::waiting;
                }
                frame.acquire_pending = false;
                frame.submitted = 0;
            }
            for (auto& image : images_) {
                if (image.present_pending &&
                    !wait_fence(image.present_fence, remaining(deadline))) {
                    return detail::DrainStatus::waiting;
                }
                image.present_pending = false;
            }
            if (!wait_queue_idle(remaining(deadline), true)) {
                return detail::DrainStatus::waiting;
            }
            // Clears NVRHI's submitted resource references only after the
            // timeline proves completion. Native image views remain owned by
            // this backend until the coordinator subsequently destroys them.
            if (validation_) {
                validation_->runGarbageCollection();
            }
            offscreen_.reset();
            return detail::DrainStatus::complete;
        } catch (Error& error) {
            if (device_lost_) {
                offscreen_.reset();
            }
            return device_lost_ ? Result<detail::DrainStatus>{detail::DrainStatus::device_lost}
                                : Result<detail::DrainStatus>{std::move(error)};
        } catch (const vk::DeviceLostError&) {
            device_lost_ = true;
            offscreen_.reset();
            return detail::DrainStatus::device_lost;
        }
    }

    detail::GraphicsResourceSnapshot presentation_resources() const noexcept override {
        detail::GraphicsResourceSnapshot result;
        result.live_swapchains = swapchain_ != VK_NULL_HANDLE ? 1U : 0U;
        result.pending_queue_workers = queue_waiter_.pending() ? 1U : 0U;
        if (offscreen_) {
            result.offscreen_resources = offscreen_->created;
            result.offscreen_submissions = offscreen_->submitted != 0 ? 1U : 0U;
            result.offscreen_recordings = offscreen_->unretired_recording ? 1U : 0U;
        }
        for (const auto& frame : frames_) {
            result.command_lists += frame.commands ? 1U : 0U;
            result.acquire_semaphores += frame.acquired != VK_NULL_HANDLE ? 1U : 0U;
            result.acquire_fences += frame.acquire_fence != VK_NULL_HANDLE ? 1U : 0U;
            result.retained_recordings += frame.unretired_recording ? 1U : 0U;
        }
        for (const auto& image : images_) {
            if (image.draw) {
                result.window_draw_resources += image.draw->created;
                result.window_draw_submissions += image.draw->submitted != 0 ? 1U : 0U;
            }
            result.image_wrappers += image.texture ? 1U : 0U;
            result.image_views += image.view != VK_NULL_HANDLE ? 1U : 0U;
            result.present_semaphores += image.rendered != VK_NULL_HANDLE ? 1U : 0U;
            result.present_fences += image.present_fence != VK_NULL_HANDLE ? 1U : 0U;
        }
        return result;
    }

    void destroy_swapchain() noexcept override {
        record_teardown();
        if (queue_waiter_.pending()) {
            std::terminate();
        }
        for (auto& image : images_) {
            image.draw.reset();
            image.texture = nullptr;
            if (image.view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_, image.view, nullptr);
            }
            if (image.rendered != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, image.rendered, nullptr);
            }
            if (image.present_fence != VK_NULL_HANDLE) {
                vkDestroyFence(device_, image.present_fence, nullptr);
            }
        }
        images_.clear();
        captured_image_.reset();
        if (swapchain_ != VK_NULL_HANDLE) {
            // All application uses and, if enabled, present-fence payloads
            // have completed. The legacy queue-idle worker has also joined.
            // With no present wait semaphores on the fallback,
            // the implementation retains presented image memory until the
            // presentation engine releases it (vkDestroySwapchainKHR contract).
            vkDestroySwapchainKHR(device_, swapchain_, nullptr);
            swapchain_ = VK_NULL_HANDLE;
        }
        extent_ = {};
    }

    void destroy_presentation() noexcept override {
        record_teardown();
        for (auto& frame : frames_) {
            frame.commands = nullptr;
            if (frame.acquire_fence != VK_NULL_HANDLE) {
                vkDestroyFence(device_, frame.acquire_fence, nullptr);
            }
            if (frame.acquired != VK_NULL_HANDLE) {
                vkDestroySemaphore(device_, frame.acquired, nullptr);
            }
            frame = {};
        }
        window_draw_.reset();
    }

    void destroy_nvrhi() noexcept override {
        record_teardown();
        if (offscreen_) {
            std::terminate();
        }
        // Configuration may have succeeded without any present attempt, in
        // which case the presentation phase itself was never initialized.
        window_draw_.reset();
        // Presentation has already completed its bounded drain or reported
        // device loss, and any legacy queue-idle worker has joined. NVRHI's
        // unbounded waitForIdle is never called on the owning thread.
        validation_ = nullptr;
        nvrhi_ = nullptr;
        nvrhi_diagnostics_.sink = nullptr;
    }

    void destroy_device() noexcept override {
        record_teardown();
        if (queue_waiter_.pending()) {
            std::terminate();
        }
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        queue_ = VK_NULL_HANDLE;
        selected_ = VK_NULL_HANDLE;
    }

    void destroy_surface() noexcept override {
        record_teardown();
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
    }

    void destroy_instance() noexcept override {
        record_teardown();
        if (messenger_ != VK_NULL_HANDLE && destroy_messenger_ != nullptr) {
            destroy_messenger_(instance_, messenger_, nullptr);
            messenger_ = VK_NULL_HANDLE;
        }
        if (instance_ != VK_NULL_HANDLE) {
            // The pNext creation callback remains live for instance teardown;
            // the owning diagnostic sink outlives this backend and all stages.
            vkDestroyInstance(instance_, nullptr);
            instance_ = VK_NULL_HANDLE;
        }
        destroy_messenger_ = nullptr;
    }

    void destroy_window() noexcept override {
        record_teardown();
        window_.reset();
    }

  private:
    void record_teardown() noexcept {
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
        if (test_) {
            ++test_->teardown_calls;
        }
#endif
    }
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
    bool inject(detail::NativeGraphicsFault fault) noexcept {
        if (test_ && test_->armed && !test_->fired && test_->fault == fault) {
            test_->fired = true;
            return true;
        }
        return false;
    }
    detail::NativeGraphicsTestControl* test_{nullptr};
#endif
    using Clock = std::chrono::steady_clock;
    static Clock::time_point make_deadline(std::uint64_t timeout_ns) noexcept {
        // The coordinator supplies millisecond/one-second bounds. Clamp this
        // private seam too, avoiding duration overflow from malformed fixtures.
        return Clock::now() + std::chrono::nanoseconds(std::min(timeout_ns, 1'000'000'000ULL));
    }
    static std::uint64_t remaining(Clock::time_point deadline) noexcept {
        const auto now = Clock::now();
        return now >= deadline
                   ? 0
                   : static_cast<std::uint64_t>(
                         std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now)
                             .count());
    }
    VkResult observe(VkResult status) noexcept {
        device_lost_ = device_lost_ || status == VK_ERROR_DEVICE_LOST;
        return status;
    }
    void check(VkResult status, std::string_view operation) {
        require_vk(observe(status), operation);
    }
    bool wait_queue_idle(std::uint64_t timeout_ns, bool start_if_needed) {
        if (!queue_waiter_.pending()) {
            if (!start_if_needed || !legacy_present_pending_ || device_lost_) {
                return true;
            }
            // Unextended WSI offers no present fence. This is the documented
            // legacy queue-idle compatibility route, isolated from the main
            // thread and used only for generation retirement/explicit drain.
            // It does not prove display scanout completion. There are no
            // presentation wait semaphores whose PE payload could outlive it.
            const auto queue = queue_;
            auto started =
                queue_waiter_.start([queue] { return static_cast<int>(vkQueueWaitIdle(queue)); });
            if (!started) {
                throw started.error();
            }
        }
        auto waited = queue_waiter_.wait(timeout_ns);
        if (!waited) {
            throw waited.error();
        }
        if (!waited.value()) {
            return false;
        }
        const auto status = observe(static_cast<VkResult>(*waited.value()));
        check(status, "graphics.presentation.queue_idle");
        legacy_present_pending_ = false;
        return true;
    }
    bool wait_fence(VkFence fence, std::uint64_t timeout_ns) {
        const auto status = observe(vkWaitForFences(device_, 1, &fence, VK_TRUE, timeout_ns));
        if (status == VK_TIMEOUT) {
            return false;
        }
        check(status, "graphics.presentation.wait_fence");
        return true;
    }
    bool wait_timeline(std::uint64_t value, std::uint64_t timeout_ns) {
        if (value == 0) {
            return true;
        }
        const auto semaphore = nvrhi_->getQueueSemaphore(nvrhi::CommandQueue::Graphics);
        VkSemaphoreWaitInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1;
        info.pSemaphores = &semaphore;
        info.pValues = &value;
        const auto status = observe(vkWaitSemaphores(device_, &info, timeout_ns));
        if (status == VK_TIMEOUT) {
            return false;
        }
        check(status, "graphics.presentation.wait_submission");
        return true;
    }
    struct Frame {
        VkSemaphore acquired{VK_NULL_HANDLE};
        VkFence acquire_fence{VK_NULL_HANDLE};
        nvrhi::CommandListHandle commands;
        std::uint64_t submitted{0};
        bool acquire_pending{false};
        bool unretired_recording{false};
    };
    struct Image {
        VkImageView view{VK_NULL_HANDLE};
        nvrhi::TextureHandle texture;
        std::unique_ptr<detail::OffscreenResources> draw;
        VkSemaphore rendered{VK_NULL_HANDLE};
        VkFence present_fence{VK_NULL_HANDLE};
        bool present_pending{false};
        bool initialized{false};
        bool uploaded{false};
    };
    std::array<Frame, 2> frames_{};
    std::unique_ptr<detail::OffscreenResources> offscreen_;
    std::optional<OffscreenRequest> window_draw_;
    std::optional<std::uint32_t> captured_image_;
    bool capture_enabled_{false};
    std::vector<Image> images_;
    VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
    VkExtent2D extent_{};
    bool device_lost_{false};
    bool legacy_present_pending_{false};
    detail::GraphicsQueueWaiter queue_waiter_;
    std::unique_ptr<Window> window_;
    VkInstance instance_{VK_NULL_HANDLE};
    VkDebugUtilsMessengerEXT messenger_{VK_NULL_HANDLE};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger_{nullptr};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    std::vector<const char*> instance_extensions_;
    std::vector<const char*> device_extensions_;
    std::vector<VkPhysicalDevice> physical_devices_;
    std::vector<GraphicsAdapter> adapters_;
    VkPhysicalDevice selected_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue queue_{VK_NULL_HANDLE};
    std::uint32_t queue_family_{0};
    bool surface_maintenance_{false};
    bool present_fences_{false};
    NvrhiDiagnostics nvrhi_diagnostics_;
    nvrhi::vulkan::DeviceHandle nvrhi_;
    nvrhi::DeviceHandle validation_;
};

} // namespace

bool native_graphics_available() noexcept {
    return true;
}

namespace detail {
std::unique_ptr<GraphicsBackend> make_graphics_backend() {
    return std::make_unique<VulkanBackend>();
}
#if defined(SHARK_GRAPHICS_TEST_HOOKS)
std::unique_ptr<GraphicsBackend> make_fault_graphics_backend(NativeGraphicsTestControl& control) {
    return std::make_unique<VulkanBackend>(control);
}
#endif
} // namespace detail
} // namespace shark::client
