/**
 * hal_vulkan.c — Vulkan GPU device detection and query
 *
 * Pure C11 HAL backend for detecting Vulkan-capable GPUs and querying device properties.
 * This module only handles DETECTION, not actual inference (llama.cpp handles that).
 *
 * Follows the same pattern as hal_scalar.c, hal_x86_avx2.c, etc.
 */

#ifdef NEURONOS_HAS_VULKAN

#include <neuronos/neuronos_hal.h>
#include <vulkan/vulkan.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    bool available;
    char device_name[256];
    VkPhysicalDeviceType device_type;
    size_t vram_bytes;
    uint32_t vendor_id;  // 0x10DE = NVIDIA, 0x1002 = AMD, 0x8086 = Intel
    uint32_t device_id;

    /* Extended properties for optimization */
    uint32_t api_version;        // Vulkan API version (VK_MAKE_VERSION format)
    uint32_t driver_version;     // Driver version (vendor-specific encoding)
    uint32_t max_compute_work_group_count[3];  // Max work groups in each dimension
    uint32_t max_compute_work_group_size[3];   // Max work group size
    uint32_t max_compute_work_group_invocations; // Max total invocations
    bool supports_fp16;          // FP16 compute support
    bool supports_int8;          // INT8 compute support
} neuronos_vulkan_device_t;

// Singleton: global Vulkan device info
static neuronos_vulkan_device_t g_vk_device = {0};
static bool g_vk_initialized = false;

/**
 * Initialize Vulkan detection (called once, lazy)
 * Creates a minimal Vulkan instance, enumerates devices, and stores info.
 */
neuronos_hal_status_t neuronos_hal_vulkan_init(void) {
    if (g_vk_initialized) return NEURONOS_HAL_OK;

    // Create Vulkan instance (minimal, no validation layers or extensions)
    VkApplicationInfo app_info = {0};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "NeuronOS";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 9, 1);
    app_info.pEngineName = "NeuronOS HAL";
    app_info.engineVersion = VK_MAKE_VERSION(0, 9, 1);
    app_info.apiVersion = VK_API_VERSION_1_0;

    VkInstanceCreateInfo create_info = {0};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledLayerCount = 0;
    create_info.enabledExtensionCount = 0;

    VkInstance instance;
    VkResult result = vkCreateInstance(&create_info, NULL, &instance);

    if (result != VK_SUCCESS) {
        // Vulkan not available (no driver, SDK issue, etc.)
        g_vk_device.available = false;
        g_vk_initialized = true;
        return NEURONOS_OK; // Not an error, just no Vulkan
    }

    // Enumerate physical devices
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, NULL);

    if (device_count == 0) {
        vkDestroyInstance(instance, NULL);
        g_vk_device.available = false;
        g_vk_initialized = true;
        return NEURONOS_OK;
    }

    // Get all devices, select best one (prefer discrete GPU)
    VkPhysicalDevice* devices = malloc(device_count * sizeof(VkPhysicalDevice));
    if (!devices) {
        vkDestroyInstance(instance, NULL);
        g_vk_device.available = false;
        g_vk_initialized = true;
        return NEURONOS_HAL_OK;
    }
    vkEnumeratePhysicalDevices(instance, &device_count, devices);

    VkPhysicalDevice selected = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < device_count; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);

        // Prefer discrete GPU over integrated
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            selected = devices[i];
            break;
        }
    }

    // Fallback to first device if no discrete GPU found
    if (selected == VK_NULL_HANDLE) {
        selected = devices[0];
    }

    // Query and store device properties
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceProperties(selected, &props);
    vkGetPhysicalDeviceMemoryProperties(selected, &mem_props);

    strncpy(g_vk_device.device_name, props.deviceName, sizeof(g_vk_device.device_name) - 1);
    g_vk_device.device_type = props.deviceType;
    g_vk_device.vendor_id = props.vendorID;
    g_vk_device.device_id = props.deviceID;
    g_vk_device.available = true;

    /* Extended properties */
    g_vk_device.api_version = props.apiVersion;
    g_vk_device.driver_version = props.driverVersion;

    /* Compute capabilities - important for shader optimization */
    for (int i = 0; i < 3; i++) {
        g_vk_device.max_compute_work_group_count[i] = props.limits.maxComputeWorkGroupCount[i];
        g_vk_device.max_compute_work_group_size[i] = props.limits.maxComputeWorkGroupSize[i];
    }
    g_vk_device.max_compute_work_group_invocations = props.limits.maxComputeWorkGroupInvocations;

    /* Query FP16 and INT8 support via features */
    VkPhysicalDeviceFeatures features;
    vkGetPhysicalDeviceFeatures(selected, &features);
    g_vk_device.supports_fp16 = features.shaderFloat64;  // Proxy for FP16 (real check needs extensions)
    g_vk_device.supports_int8 = features.shaderInt16;    // Proxy for INT8

    // Calculate total VRAM (sum all device-local memory heaps)
    g_vk_device.vram_bytes = 0;
    for (uint32_t i = 0; i < mem_props.memoryHeapCount; i++) {
        if (mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            g_vk_device.vram_bytes += mem_props.memoryHeaps[i].size;
        }
    }

    // Cleanup: we only needed detection, llama.cpp will manage actual Vulkan usage
    free(devices);
    vkDestroyInstance(instance, NULL);

    g_vk_initialized = true;
    return NEURONOS_HAL_OK;
}

/**
 * Get Vulkan device info (const pointer to singleton)
 */
const neuronos_vulkan_device_t* neuronos_hal_vulkan_get_device(void) {
    if (!g_vk_initialized) {
        neuronos_hal_vulkan_init();
    }
    return &g_vk_device;
}

/**
 * Print Vulkan device info (for `neuronos hwinfo` command)
 */
void neuronos_hal_vulkan_print_info(void) {
    const neuronos_vulkan_device_t* dev = neuronos_hal_vulkan_get_device();

    if (!dev->available) {
        printf("Vulkan GPU: Not available\n");
        return;
    }

    // Device type string
    const char* type_str = "Unknown";
    switch (dev->device_type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   type_str = "Discrete";   break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: type_str = "Integrated"; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    type_str = "Virtual";    break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            type_str = "CPU";        break;
        default: break;
    }

    printf("Vulkan GPU: %s (%s)\n", dev->device_name, type_str);
    printf("  VRAM: %zu MB\n", dev->vram_bytes / (1024 * 1024));

    // Vendor name
    printf("  Vendor: 0x%04X ", dev->vendor_id);
    if (dev->vendor_id == 0x10DE) {
        printf("(NVIDIA)");
    } else if (dev->vendor_id == 0x1002) {
        printf("(AMD)");
    } else if (dev->vendor_id == 0x8086) {
        printf("(Intel)");
    }
    printf("\n");

    printf("  Device ID: 0x%04X\n", dev->device_id);

    /* Extended info - useful for optimization */
    printf("  Vulkan API: %u.%u.%u\n",
           VK_VERSION_MAJOR(dev->api_version),
           VK_VERSION_MINOR(dev->api_version),
           VK_VERSION_PATCH(dev->api_version));

    /* Driver version (vendor-specific encoding) */
    if (dev->vendor_id == 0x10DE) {
        /* NVIDIA: major.minor.patch */
        printf("  Driver: %u.%u.%u (NVIDIA)\n",
               (dev->driver_version >> 22) & 0x3FF,
               (dev->driver_version >> 14) & 0xFF,
               (dev->driver_version >> 6) & 0xFF);
    } else {
        /* Generic VK_MAKE_VERSION format */
        printf("  Driver: %u.%u.%u\n",
               VK_VERSION_MAJOR(dev->driver_version),
               VK_VERSION_MINOR(dev->driver_version),
               VK_VERSION_PATCH(dev->driver_version));
    }

    /* Compute capabilities */
    printf("  Compute: WorkGroups=%ux%ux%u, Size=%ux%ux%u, Invocations=%u\n",
           dev->max_compute_work_group_count[0],
           dev->max_compute_work_group_count[1],
           dev->max_compute_work_group_count[2],
           dev->max_compute_work_group_size[0],
           dev->max_compute_work_group_size[1],
           dev->max_compute_work_group_size[2],
           dev->max_compute_work_group_invocations);
}

#else
// Vulkan support not compiled in
#include <neuronos/neuronos_hal.h>
#include <stdio.h>
#include <stddef.h>

neuronos_hal_status_t neuronos_hal_vulkan_init(void) {
    return NEURONOS_HAL_OK;
}

const void* neuronos_hal_vulkan_get_device(void) {
    return NULL;
}

void neuronos_hal_vulkan_print_info(void) {
    printf("Vulkan GPU: Not compiled (build with -DNEURONOS_VULKAN=ON)\n");
}

#endif // NEURONOS_HAS_VULKAN
