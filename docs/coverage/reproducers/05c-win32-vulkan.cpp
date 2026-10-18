// Diagnostic only: eight Win32/Vulkan swapchain lifetimes on an explicit GPU.
// No Engine, GLFW or NVRHI code and no acquired images or submitted GPU work.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
#include <windows.h>

static std::atomic<unsigned> diagnostics{0};
static unsigned cleanup_errors = 0;

static DWORD gui_resources(DWORD kind) {
    SetLastError(ERROR_SUCCESS);
    const DWORD count = GetGuiResources(GetCurrentProcess(), kind);
    if (count == 0 && GetLastError() != ERROR_SUCCESS)
        throw std::runtime_error("GUI resource query failed");
    return count;
}

static std::string quoted(const char* text) {
    std::string result{"\""};
    for (; *text; ++text) {
        const auto value = static_cast<unsigned char>(*text);
        if (value == '"' || value == '\\')
            result += '\\';
        if (value < 32) {
            char escaped[7]{};
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(value));
            result += escaped;
        } else
            result += *text;
    }
    result += '"';
    return result;
}

static void pump() {
    MSG message{};
    for (unsigned count = 0; count < 4096; ++count) {
        if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            return;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    throw std::runtime_error("Window message pump exceeded fixed bound");
}
static VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT,
                                                 const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                 void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++diagnostics;
        std::fprintf(stderr, "%s\n", data->pMessage);
    }
    return VK_FALSE;
}
static void check(VkResult result) {
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan operation failed");
}
static void sample(unsigned cycle, const char* stage) {
    GdiFlush();
    DWORD handles = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &handles))
        throw std::runtime_error("handle count failed");
    std::printf("{\"kind\":\"sample\",\"cycle\":%u,\"stage\":\"%s\",\"gdi\":%lu,\"user\":%lu,"
                "\"handles\":%lu,\"diagnostics\":%u,\"cleanup_errors\":%u}\n",
                cycle, stage, gui_resources(GR_GDIOBJECTS), gui_resources(GR_USEROBJECTS), handles,
                diagnostics.load(), cleanup_errors);
    if (std::fflush(stdout) != 0 || std::ferror(stdout))
        throw std::runtime_error("Cannot write diagnostic trace");
}
struct Owner {
    HWND window{};
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    PFN_vkDestroyDebugUtilsMessengerEXT destroy_messenger{};
    VkSurfaceKHR surface{};
    VkDevice device{};
    VkSwapchainKHR swapchain{};
    void close() noexcept {
        // No acquisition, command recording, queue submission or presentation occurs.
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (device)
            vkDestroyDevice(device, nullptr);
        if (surface)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (messenger)
            destroy_messenger(instance, messenger, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
        swapchain = {};
        device = {};
        surface = {};
        messenger = {};
        instance = {};
        if (window) {
            if (DestroyWindow(window))
                window = {};
            else
                ++cleanup_errors;
        }
    }
    ~Owner() {
        close();
    }
};
struct RegisteredClass {
    const wchar_t* name;
    HINSTANCE instance;
    bool active{true};
    void close() noexcept {
        if (active) {
            if (UnregisterClassW(name, instance))
                active = false;
            else
                ++cleanup_errors;
        }
    }
    ~RegisteredClass() {
        close();
    }
};
int main(int argc, char** argv) {
    try {
        if (argc != 2 || std::strlen(argv[1]) != 32)
            throw std::runtime_error("explicit UUID required");
        for (const char* digit = argv[1]; *digit; ++digit)
            if (!((*digit >= '0' && *digit <= '9') || (*digit >= 'a' && *digit <= 'f')))
                throw std::runtime_error("lowercase hexadecimal UUID required");
        std::printf("{\"kind\":\"identity\",\"schema_version\":1,\"qualification\":\"NOT_RUN\","
                    "\"validation\":\"required\",\"selection\":\"%s\",\"lifetimes\":8,\"acquired\":"
                    "0,\"submitted\":0,\"presented\":0}\n",
                    argv[1]);
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_OWNDC;
        window_class.lpfnWndProc = DefWindowProcW;
        window_class.hInstance = GetModuleHandleW(nullptr);
        window_class.lpszClassName = L"SharkRawVulkanGdiDiagnostic";
        if (!RegisterClassExW(&window_class))
            throw std::runtime_error("class registration failed");
        RegisteredClass registration{window_class.lpszClassName, window_class.hInstance};
        sample(0, "initial");
        for (unsigned cycle = 1; cycle <= 8; ++cycle) {
            Owner owner;
            owner.window = CreateWindowExW(0, window_class.lpszClassName, L"Raw Vulkan diagnostic",
                                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640,
                                           400, nullptr, nullptr, window_class.hInstance, nullptr);
            if (!owner.window)
                throw std::runtime_error("window creation failed");
            ShowWindow(owner.window, SW_SHOWNOACTIVATE);
            pump();
            const char* extensions[]{
                VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
            const char* layers[]{"VK_LAYER_KHRONOS_validation"};
            VkDebugUtilsMessengerCreateInfoEXT debug{
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = diagnostic;
            VkValidationFeatureEnableEXT feature =
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
            VkValidationFeaturesEXT validation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
            validation.pNext = &debug;
            validation.enabledValidationFeatureCount = 1;
            validation.pEnabledValidationFeatures = &feature;
            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            app.pApplicationName = "SharkRawVulkanGdiDiagnostic";
            app.apiVersion = VK_API_VERSION_1_3;
            VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
            info.pNext = &validation;
            info.pApplicationInfo = &app;
            info.enabledExtensionCount = 4;
            info.ppEnabledExtensionNames = extensions;
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = layers;
            check(vkCreateInstance(&info, nullptr, &owner.instance));
            const auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(owner.instance, "vkCreateDebugUtilsMessengerEXT"));
            owner.destroy_messenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(owner.instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (!create_messenger || !owner.destroy_messenger)
                throw std::runtime_error("debug utilities unavailable");
            check(create_messenger(owner.instance, &debug, nullptr, &owner.messenger));
            VkWin32SurfaceCreateInfoKHR surface_info{
                VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
            surface_info.hinstance = window_class.hInstance;
            surface_info.hwnd = owner.window;
            check(vkCreateWin32SurfaceKHR(owner.instance, &surface_info, nullptr, &owner.surface));
            uint32_t count = 0;
            check(vkEnumeratePhysicalDevices(owner.instance, &count, nullptr));
            if (count == 0 || count > 32)
                throw std::runtime_error("device count out of bounds");
            std::vector<VkPhysicalDevice> devices(count);
            check(vkEnumeratePhysicalDevices(owner.instance, &count, devices.data()));
            devices.resize(count);
            VkPhysicalDevice selected{};
            for (const auto physical : devices) {
                VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
                VkPhysicalDeviceDriverProperties driver{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                id.pNext = &driver;
                VkPhysicalDeviceProperties2 properties{
                    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
                properties.pNext = &id;
                vkGetPhysicalDeviceProperties2(physical, &properties);
                char uuid[33]{};
                for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
                    std::snprintf(uuid + i * 2, 3, "%02x", static_cast<unsigned>(id.deviceUUID[i]));
                if (std::strcmp(uuid, argv[1]) == 0) {
                    if (selected)
                        throw std::runtime_error("duplicate adapter UUID");
                    selected = physical;
                    std::printf("{\"kind\":\"adapter\",\"cycle\":%u,\"device\":%s,\"uuid\":\"%s\","
                                "\"driver_version\":%u,\"driver_name\":%s,\"driver_info\":%s}\n",
                                cycle, quoted(properties.properties.deviceName).c_str(), uuid,
                                properties.properties.driverVersion,
                                quoted(driver.driverName).c_str(),
                                quoted(driver.driverInfo).c_str());
                }
            }
            if (!selected)
                throw std::runtime_error("UUID unavailable");
            vkGetPhysicalDeviceQueueFamilyProperties(selected, &count, nullptr);
            if (count == 0 || count > 256)
                throw std::runtime_error("queue family count out of bounds");
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(selected, &count, families.data());
            uint32_t queue = count;
            for (uint32_t i = 0; i < count; ++i) {
                VkBool32 supported{};
                check(vkGetPhysicalDeviceSurfaceSupportKHR(selected, i, owner.surface, &supported));
                if (supported && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                    queue = i;
                    break;
                }
            }
            if (queue == count)
                throw std::runtime_error("no presentation queue");
            float priority = 1;
            VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
            queue_info.queueFamilyIndex = queue;
            queue_info.queueCount = 1;
            queue_info.pQueuePriorities = &priority;
            const char* device_extensions[]{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
            device_info.queueCreateInfoCount = 1;
            device_info.pQueueCreateInfos = &queue_info;
            device_info.enabledExtensionCount = 1;
            device_info.ppEnabledExtensionNames = device_extensions;
            check(vkCreateDevice(selected, &device_info, nullptr, &owner.device));
            VkSurfaceCapabilitiesKHR caps{};
            check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(selected, owner.surface, &caps));
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(selected, owner.surface, &count, nullptr));
            if (count == 0 || count > 256)
                throw std::runtime_error("format count out of bounds");
            std::vector<VkSurfaceFormatKHR> formats(count);
            check(vkGetPhysicalDeviceSurfaceFormatsKHR(selected, owner.surface, &count,
                                                       formats.data()));
            formats.resize(count);
            VkSurfaceFormatKHR format{};
            for (const auto& candidate : formats)
                if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB &&
                    candidate.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    format = candidate;
            if (!format.format || caps.currentExtent.width == UINT32_MAX ||
                !caps.currentExtent.width || !caps.currentExtent.height ||
                !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ||
                !(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
                throw std::runtime_error("surface requirement missing");
            VkSwapchainCreateInfoKHR swap{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
            swap.surface = owner.surface;
            swap.minImageCount = caps.minImageCount;
            swap.imageFormat = format.format;
            swap.imageColorSpace = format.colorSpace;
            swap.imageExtent = caps.currentExtent;
            swap.imageArrayLayers = 1;
            swap.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            swap.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
            swap.preTransform = caps.currentTransform;
            swap.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
            swap.presentMode = VK_PRESENT_MODE_FIFO_KHR;
            swap.clipped = VK_TRUE;
            sample(cycle, "before_swapchain");
            check(vkCreateSwapchainKHR(owner.device, &swap, nullptr, &owner.swapchain));
            sample(cycle, "after_swapchain");
            owner.close();
            pump();
            sample(cycle, "closed");
        }
        registration.close();
        sample(8, "final");
        return diagnostics.load() == 0 && cleanup_errors == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
