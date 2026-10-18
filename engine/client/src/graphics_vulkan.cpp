#include "graphics_backend.hpp"

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
#include <memory>
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
            VkPhysicalDeviceVulkan12Features enabled12{};
            enabled12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            enabled12.pNext = &enabled13;
            enabled12.timelineSemaphore = VK_TRUE;
            device_extensions_ = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
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

    void destroy_nvrhi() noexcept override {
        // No commands can be created or submitted in 05a. All NVRHI-owned
        // initialization objects may therefore be destroyed without a GPU wait.
        validation_ = nullptr;
        nvrhi_ = nullptr;
        nvrhi_diagnostics_.sink = nullptr;
    }

    void destroy_device() noexcept override {
        if (device_ != VK_NULL_HANDLE) {
            vkDestroyDevice(device_, nullptr);
            device_ = VK_NULL_HANDLE;
        }
        queue_ = VK_NULL_HANDLE;
        selected_ = VK_NULL_HANDLE;
    }

    void destroy_surface() noexcept override {
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(instance_, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
    }

    void destroy_instance() noexcept override {
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
        window_.reset();
    }

  private:
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
} // namespace detail
} // namespace shark::client
