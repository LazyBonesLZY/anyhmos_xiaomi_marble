/* Standalone, offscreen probe for ADY-AL10 207.0.0.111(SP10) on marble.
 * No window, scanout, service hook, or output file. 0 = both colors verified,
 * 1 = API/setup/cleanup failed, 2 = fence(s) signalled but pixels did not match.
 */
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Deliberately not square and not a tightly aligned RGBA row. */
enum { WIDTH = 37, HEIGHT = 23, RGBA_BYTES = 4 };

static int covers_pixels(uint64_t pitch, uint64_t bytes)
{
    const uint64_t row = WIDTH * RGBA_BYTES;
    /* Check the last visible pixel without reading padding or overflowing. */
    return bytes <= SIZE_MAX && bytes >= row && pitch >= row &&
           pitch <= (bytes - row) / (HEIGHT - 1);
}

static int check_pixels(const void *map, uint64_t pitch, uint64_t bytes,
                        const uint8_t expected[RGBA_BYTES], unsigned pass)
{
    static const unsigned xy[][2] = {{0, 0}, {WIDTH - 1, 0}, {WIDTH / 2, HEIGHT / 2},
                                    {0, HEIGHT - 1}, {WIDTH - 1, HEIGHT - 1}};
    if (!map || !covers_pixels(pitch, bytes))
        return -1;
    unsigned match = 0, rgb_zero = 0;
    for (unsigned i = 0; i < sizeof(xy) / sizeof(xy[0]); ++i) {
        const uint8_t *p = (const uint8_t *)map + xy[i][1] * (size_t)pitch + xy[i][0] * RGBA_BYTES;
        match += memcmp(p, expected, RGBA_BYTES) == 0;
        rgb_zero += (p[0] | p[1] | p[2]) == 0;
        printf("sample pass=%u x=%u y=%u rgba=%02x%02x%02x%02x\n", pass, xy[i][0], xy[i][1],
               (unsigned)p[0], (unsigned)p[1], (unsigned)p[2], (unsigned)p[3]);
    }
    printf("pixels pass=%u expected=%02x%02x%02x%02x matches=%u/5 rgb_zero=%u/5\n", pass,
           (unsigned)expected[0], (unsigned)expected[1], (unsigned)expected[2], (unsigned)expected[3],
           match, rgb_zero);
    return match != sizeof(xy) / sizeof(xy[0]);
}

#ifndef MARBLE_VULKAN_PIXEL_TEST
#include <stdlib.h>
#define VK_USE_PLATFORM_OHOS 1
#include <vulkan/vulkan.h>
#include <native_buffer/native_buffer.h>

/* Handoff: donor CreateVkImage uses 0x8000; ADC API 20 headers say 0x2000.
 * Keep the donor value explicit. Do not retry with a different handle type.
 */
#define DONOR_HANDLE ((VkExternalMemoryHandleTypeFlagBits)0x00008000u)

static const char *const device_extensions[] = {
    VK_OHOS_EXTERNAL_MEMORY_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};

static int vk_ok(const char *stage, VkResult result)
{
    if (result != VK_SUCCESS)
        fprintf(stderr, "api_error stage=%s VkResult=%d\n", stage, (int)result);
    return result == VK_SUCCESS;
}

#define VK_CHECK(call) do { if (!vk_ok(#call, (call))) goto cleanup; } while (0)
#define REQUIRE(condition, stage) do { \
    if (!(condition)) { fprintf(stderr, "setup_error stage=%s\n", (stage)); goto cleanup; } \
} while (0)

static VkPhysicalDevice select_turnip(VkInstance instance)
{
    uint32_t count = 0;
    if (!vk_ok("enumerate_physical_count", vkEnumeratePhysicalDevices(instance, &count, NULL)) || !count)
        return VK_NULL_HANDLE;
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices)), selected = VK_NULL_HANDLE;
    if (devices && vk_ok("enumerate_physical_devices", vkEnumeratePhysicalDevices(instance, &count, devices))) {
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);
            if (props.apiVersion < VK_API_VERSION_1_2 || props.vendorID != 0x5143)
                continue;
            VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
            VkPhysicalDeviceProperties2 props2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                                  .pNext = &driver};
            vkGetPhysicalDeviceProperties2(devices[i], &props2);
            if (driver.driverID == VK_DRIVER_ID_MESA_TURNIP) {
                printf("device name=%s driver=%s driver_id=%u device_id=0x%x api=0x%x\n",
                       props.deviceName, driver.driverName, (unsigned)driver.driverID, props.deviceID, props.apiVersion);
                selected = devices[i];
                break;
            }
        }
    }
    free(devices);
    return selected;
}

static uint32_t graphics_queue(VkPhysicalDevice physical)
{
    uint32_t count = 0, index = UINT32_MAX;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (count && families) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
        for (uint32_t i = 0; i < count; ++i) {
            if (families[i].queueCount && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                index = i;
                break;
            }
        }
    }
    free(families);
    return index;
}

static int probe(void)
{
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    OH_NativeBuffer *native = NULL;
    int status = 1, mismatch = 0;

    printf("abi=ADY-AL10-207.0.0.111-SP10 handle=0x%x sdk_handle=0x%x extent=%dx%d\n",
           (unsigned)DONOR_HANDLE, (unsigned)VK_EXTERNAL_MEMORY_HANDLE_TYPE_OHOS_NATIVE_BUFFER_BIT_OHOS,
           WIDTH, HEIGHT);
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .pApplicationName = "marble_vulkan_native_clear", .apiVersion = VK_API_VERSION_1_2};
    const VkInstanceCreateInfo instance_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                                .pApplicationInfo = &app};
    VK_CHECK(vkCreateInstance(&instance_info, NULL, &instance));
    VkPhysicalDevice physical = select_turnip(instance);
    REQUIRE(physical, "Mesa Turnip Vulkan 1.2+ not enumerated by OHOS loader");
    uint32_t family = graphics_queue(physical);
    REQUIRE(family != UINT32_MAX, "graphics queue not found");

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority};
    const VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = sizeof(device_extensions) / sizeof(device_extensions[0]),
        .ppEnabledExtensionNames = device_extensions};
    /* The loader must enable both extensions; a missing one is an API error. */
    VK_CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    PFN_vkGetNativeBufferPropertiesOHOS get_native_properties =
        (PFN_vkGetNativeBufferPropertiesOHOS)vkGetDeviceProcAddr(device, "vkGetNativeBufferPropertiesOHOS");
    REQUIRE(get_native_properties, "vkGetNativeBufferPropertiesOHOS dispatch missing");
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkPhysicalDeviceExternalImageFormatInfo external_query = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, .handleType = DONOR_HANDLE};
    const VkPhysicalDeviceImageFormatInfo2 query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &external_query, .format = format, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = usage};
    VkExternalImageFormatProperties external = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 support = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &external};
    VK_CHECK(vkGetPhysicalDeviceImageFormatProperties2(physical, &query, &support));
    REQUIRE((external.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
            (external.externalMemoryProperties.compatibleHandleTypes & DONOR_HANDLE) &&
            support.imageFormatProperties.maxExtent.width >= WIDTH && support.imageFormatProperties.maxExtent.height >= HEIGHT &&
            (support.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT), "donor linear RGBA import unsupported");

    const OH_NativeBuffer_Config request = {.width = WIDTH, .height = HEIGHT, .format = NATIVEBUFFER_PIXEL_FMT_RGBA_8888,
        .usage = NATIVEBUFFER_USAGE_MEM_DMA | NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_CPU_READ};
    native = OH_NativeBuffer_Alloc(&request);
    REQUIRE(native, "OH_NativeBuffer_Alloc");
    OH_NativeBuffer_Config actual = {0};
    OH_NativeBuffer_GetConfig(native, &actual);
    printf("native config=%dx%d format=%d usage=0x%x stride_bytes=%d\n", actual.width, actual.height,
           actual.format, (unsigned)actual.usage, actual.stride);
    REQUIRE(actual.width == WIDTH && actual.height == HEIGHT && actual.format == request.format &&
            (actual.usage & request.usage) == request.usage && actual.stride >= WIDTH * RGBA_BYTES,
            "unexpected OH_NativeBuffer config");

    VkNativeBufferFormatPropertiesOHOS native_format = {.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_FORMAT_PROPERTIES_OHOS};
    VkNativeBufferPropertiesOHOS native_props = {.sType = VK_STRUCTURE_TYPE_NATIVE_BUFFER_PROPERTIES_OHOS,
                                                .pNext = &native_format};
    VK_CHECK(get_native_properties(device, native, &native_props));
    REQUIRE(native_format.format == format && !native_format.externalFormat &&
            covers_pixels((uint32_t)actual.stride, native_props.allocationSize), "unexpected native format/allocation size");
    const VkExternalMemoryImageCreateInfo external_image = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                                          .handleTypes = DONOR_HANDLE};
    const VkImageCreateInfo image_info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_image, .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = {WIDTH, HEIGHT, 1},
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
        .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(device, &image_info, NULL, &image));

    /* Native import properties provide the allocation size and type bits;
     * Turnip has not computed the image's actual layout before bind.
     */
    const uint32_t type_bits = native_props.memoryTypeBits;
    REQUIRE(type_bits, "no native import memory type");
    const VkMemoryDedicatedAllocateInfo dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = image};
    const VkImportNativeBufferInfoOHOS import = {.sType = VK_STRUCTURE_TYPE_IMPORT_NATIVE_BUFFER_INFO_OHOS,
                                               .pNext = &dedicated, .buffer = native};
    const VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = native_props.allocationSize, .memoryTypeIndex = (uint32_t)__builtin_ctz(type_bits)};
    VK_CHECK(vkAllocateMemory(device, &alloc, NULL, &memory));
    const VkBindImageMemoryInfo bind = {.sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, .image = image, .memory = memory};
    VK_CHECK(vkBindImageMemory2(device, 1, &bind));

    /* Turnip learns the external row pitch at bind time. Check before sampling. */
    const VkImageSubresource subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT};
    VkSubresourceLayout layout = {0};
    vkGetImageSubresourceLayout(device, image, &subresource, &layout);
    printf("layout offset=%" PRIu64 " pitch=%" PRIu64 " span=%" PRIu64 " allocation=%" PRIu64 "\n",
           layout.offset, layout.rowPitch, layout.size, native_props.allocationSize);
    REQUIRE(layout.offset == 0 && layout.rowPitch == (uint32_t)actual.stride &&
            layout.size <= native_props.allocationSize && covers_pixels(layout.rowPitch, layout.size),
            "unexpected bound Vulkan linear layout");

    const VkCommandPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                               .queueFamilyIndex = family};
    VK_CHECK(vkCreateCommandPool(device, &pool_info, NULL, &pool));
    const VkCommandBufferAllocateInfo cmd_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(device, &cmd_info, &cmd));
    const VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device, &fence_info, NULL, &fence));

    /* Same DMA-BUF, independent clears: a stale map from pass 1 fails pass 2. */
    const uint8_t colors[][RGBA_BYTES] = {{255, 0, 0, 255}, {0, 255, 255, 255}};
    const VkImageSubresourceRange range = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1};
    for (unsigned i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        printf("clear pass=%u\n", i + 1);
        VK_CHECK(vkResetCommandPool(device, pool, 0));
        const VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                               .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        VK_CHECK(vkBeginCommandBuffer(cmd, &begin));
        const VkImageMemoryBarrier acquire = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = i ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT,
            .dstQueueFamilyIndex = family, .image = image, .subresourceRange = range};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &acquire);
        VkClearColorValue color;
        for (unsigned c = 0; c < RGBA_BYTES; ++c)
            color.float32[c] = colors[i][c] / 255.0f;
        vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        const VkImageMemoryBarrier release = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = family,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .image = image, .subresourceRange = range};
        const VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, NULL, 1, &release);
        VK_CHECK(vkEndCommandBuffer(cmd));
        VK_CHECK(vkResetFences(device, 1, &fence));
        const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        if (!vk_ok("vkWaitForFences (10s)", vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)))) {
            /* Completion is unknown: do not free potentially in-use objects. Exit the process. */
            return 1;
        }
        printf("vulkan pass=%u submit=0 fence=0\n", i + 1);

        void *map = NULL;
        OH_NativeBuffer_Planes planes = {0};
        int32_t map_rc = OH_NativeBuffer_MapPlanes(native, &map, &planes);
        if (map_rc) {
            fprintf(stderr, "api_error stage=OH_NativeBuffer_MapPlanes result=%d\n", map_rc);
            goto cleanup;
        }
        /* Public map metadata must agree with Vulkan; never guess a row pitch. */
        int pixels = -1;
        if (planes.planeCount == 1 && planes.planes[0].offset == layout.offset &&
            planes.planes[0].rowStride == layout.rowPitch && planes.planes[0].columnStride == RGBA_BYTES) {
            pixels = check_pixels(map, layout.rowPitch, layout.size, colors[i], i + 1);
        } else {
            fprintf(stderr, "mapped_layout pass=%u planes=%u offset=%" PRIu64 " row=%u column=%u\n",
                    i + 1, planes.planeCount, planes.planes[0].offset, planes.planes[0].rowStride,
                    planes.planes[0].columnStride);
        }
        int32_t unmap_rc = OH_NativeBuffer_Unmap(native);
        if (unmap_rc)
            fprintf(stderr, "api_error stage=OH_NativeBuffer_Unmap result=%d\n", unmap_rc);
        REQUIRE(pixels >= 0, "mapped RGBA plane disagrees with bound layout");
        if (unmap_rc)
            goto cleanup;
        mismatch |= pixels;
    }
    status = mismatch ? 2 : 0;

cleanup:
    if (fence) vkDestroyFence(device, fence, NULL);
    if (pool) vkDestroyCommandPool(device, pool, NULL);
    if (image) vkDestroyImage(device, image, NULL);
    if (memory) vkFreeMemory(device, memory, NULL);
    if (native) {
        int32_t rc = OH_NativeBuffer_Unreference(native);
        if (rc) {
            fprintf(stderr, "api_error stage=OH_NativeBuffer_Unreference result=%d\n", rc);
            status = 1;
        }
    }
    if (device) vkDestroyDevice(device, NULL);
    if (instance) vkDestroyInstance(instance, NULL);
    return status;
}

int main(void)
{
    const int status = probe();
    printf("result=%s exit=%d\n", status == 0 ? "verified_samples" : status == 2 ? "pixel_mismatch" : "api_or_setup_error", status);
    return status;
}
#endif
