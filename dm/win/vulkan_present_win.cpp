// Windows Vulkan 呈现：实例 / 设备 / 窗口交换链 / blit 上屏，然后把设备交给图形引擎（dm_vk_initialize）。
// 与安卓实现的分工相同：引擎只认「源图 TRANSFER_SRC_OPTIMAL + 同步 present 回调」这一条契约，呈现只做拷贝、缩放与留边；
// 差别是窗口由前端创建的 HWND 提供，交换链尺寸跟客户区走，尺寸变化由前端在 WM_SIZE 里打脏标记。
// 选设备的顺序刻意简单：先挑同时具备「图形 + 计算 + 可呈现队列族」与交换链扩展的设备，再在同分时优先非 CPU 设备。
// （Snapdragon X2 上只有 Adreno 一个 Vulkan 设备；多 GPU 机器上这样能把呈现放到独立显卡而不是微软的基本渲染驱动。）
// Windows Vulkan presentation: instance / device / window swapchain / blit to screen, then hand the device to the graphics engine (dm_vk_initialize).
// Same division of labor as the Android implementation: the engine only knows the contract "source image in TRANSFER_SRC_OPTIMAL + a synchronous
// present callback", and presentation only copies, scales and letterboxes; the difference is that the window comes from an HWND the front end
// created, the swapchain follows the client area, and the front end marks it dirty on WM_SIZE.
// Device selection is deliberately simple: first a device with a graphics + compute + presentable queue family and the swapchain extension,
// then, among equals, a non-CPU device. (Snapdragon X2 has one Vulkan device, the Adreno; on multi-GPU machines this keeps presentation on the
// discrete GPU rather than Microsoft's Basic Render Driver.)
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "../gfx/vulkan/nvn_vk.h"

extern "C" void dm_win_vk_notify_resize(void);

namespace {
std::mutex render_mutex;
HWND window = nullptr;
uint64_t generation = 0, built_generation = ~0ull;
VkInstance instance = VK_NULL_HANDLE;
VkPhysicalDevice physical = VK_NULL_HANDLE;
VkDevice device = VK_NULL_HANDLE;
VkQueue queue = VK_NULL_HANDLE;
uint32_t family = 0;
VkSurfaceKHR surface = VK_NULL_HANDLE;
VkSwapchainKHR swapchain = VK_NULL_HANDLE;
VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
VkExtent2D extent{0, 0};
std::vector<VkImage> images;
VkCommandPool command_pool = VK_NULL_HANDLE;
VkCommandBuffer command = VK_NULL_HANDLE;
VkSemaphore acquired = VK_NULL_HANDLE;
VkFence fence = VK_NULL_HANDLE;
std::vector<VkSemaphore> finished;
bool swap_dirty = true, ready = false;
// 两张在途：GTK/安卓上验证过的同一套（每张在途帧自己的池 / 获取信号量 / 栅栏），这里保留，Windows 的 DWM 同样吃得住
// Two frames in flight: the same set verified on GTK/Android (each in-flight frame has its own pool / acquire semaphore / fence); kept here,
// and the Windows DWM copes with it just as well
VkCommandPool overlap_pool = VK_NULL_HANDLE;
VkCommandBuffer overlap_command = VK_NULL_HANDLE;
VkSemaphore overlap_acquired = VK_NULL_HANDLE;
VkFence overlap_fence = VK_NULL_HANDLE;
bool in_flight[2] = {false, false};
unsigned flight = 0;
std::string shader_dir;
std::string device_name;
// 上屏帧率（标题栏用）：每次真正上屏记一次，最近约 0.75 s 的窗口算一次显示值
// Present frame rate (for the title bar): count every frame actually put on screen, publish a value per ~0.75 s window
std::atomic<int> fps_value{0};
uint64_t fps_window_start = 0;
unsigned fps_window_frames = 0;

__attribute__((format(printf, 1, 2))) void vlog(const char *format, ...) {
    char text[1100]; va_list a; va_start(a, format); int n = vsnprintf(text, sizeof text - 1, format, a); va_end(a);
    if (n < 0) return;
    size_t len = std::min<size_t>(size_t(n), sizeof text - 2);
    text[len] = '\n'; fwrite(text, 1, len + 1, stderr);
}
void log_message(void *, const char *text) { vlog("%s", text); }
[[noreturn]] void fatal(void *, const char *text) { vlog("%s", text); fflush(stderr); abort(); }

// 交换链图像尺寸 = 客户区尺寸（DWM 会按客户区拉伸交换链，所以留边必须在 blit 里做，不能靠小尺寸缓冲）
// Swapchain image size = client area size (the DWM stretches the swapchain to the client area, so letterboxing must be done in the blit, not
// by a smaller buffer)
void client_size(uint32_t *w, uint32_t *h) {
    RECT rc{0, 0, 1, 1};
    if (window) GetClientRect(window, &rc);
    *w = rc.right > 0 ? uint32_t(rc.right - rc.left) : 1u;
    *h = rc.bottom > 0 ? uint32_t(rc.bottom - rc.top) : 1u;
}

void destroy_present_semaphores() {
    for (auto s : finished) if (s) vkDestroySemaphore(device, s, nullptr);
    finished.clear();
}
void destroy_swap() {
    destroy_present_semaphores();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE; images.clear();
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
    surface = VK_NULL_HANDLE;
}
int create_surface() {
    if (!window) return VK_NOT_READY;
    VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    ci.hinstance = GetModuleHandleA(nullptr); ci.hwnd = window;
    VkResult r = vkCreateWin32SurfaceKHR(instance, &ci, nullptr, &surface);
    if (r) return r;
    VkBool32 supported = VK_FALSE;
    r = vkGetPhysicalDeviceSurfaceSupportKHR(physical, family, surface, &supported);
    if (r || !supported) return r ? r : VK_ERROR_FEATURE_NOT_PRESENT;
    return 0;
}
int create_swap() {
    uint32_t cw, ch; client_size(&cw, &ch);
    VkSurfaceCapabilitiesKHR cap;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &cap);
    if (r) return r;
    if (!(cap.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    // Windows 上没有 pre-rotate 的问题，但个别驱动不报 IDENTITY；不支持时用当前值
    // No pre-rotation issue on Windows, but a few drivers do not report IDENTITY; use the current value when unsupported
    VkSurfaceTransformFlagBitsKHR pre = (cap.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                                            ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : cap.currentTransform;
    uint32_t n = 0;
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, nullptr);
    if (r || !n) return r ? r : VK_ERROR_FORMAT_NOT_SUPPORTED;
    std::vector<VkSurfaceFormatKHR> formats(n);
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &n, formats.data());
    if (r) return r;
    VkSurfaceFormatKHR sf = formats[0];
    if (sf.format == VK_FORMAT_UNDEFINED) { sf.format = VK_FORMAT_B8G8R8A8_UNORM; sf.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR; }
    for (auto &f : formats) if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { sf = f; break; }
    extent.width = std::clamp(cw, cap.minImageExtent.width, cap.maxImageExtent.width);
    extent.height = std::clamp(ch, cap.minImageExtent.height, cap.maxImageExtent.height);
    if (!extent.width || !extent.height) return VK_NOT_READY;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = std::clamp(3u, std::max(2u, cap.minImageCount), cap.maxImageCount ? cap.maxImageCount : 3u);   // 三张图：两张交替在途 + 一张给呈现引擎读，获取图片这一步不再卡在垂直同步上 | three images: two in flight alternately plus one the presentation engine is still reading, so acquiring an image no longer blocks on vsync
    if (cap.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, cap.maxImageCount);
    ci.imageFormat = sf.format; ci.imageColorSpace = sf.colorSpace; ci.imageExtent = extent; ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = pre; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(cap.supportedCompositeAlpha & ci.compositeAlpha))
        for (uint32_t b = 1; b <= 8; b <<= 1) if (cap.supportedCompositeAlpha & b) { ci.compositeAlpha = VkCompositeAlphaFlagBitsKHR(b); break; }
    // 出图模式（DM_PRESENT_MODE=fifo|relaxed|mailbox|immediate，缺省 relaxed）：FIFO 在「差一点赶不上一个周期」时会把这一帧压到下一个垂直同步，
    // 前三项实测里营地约三成帧就是这样被压成两个周期，平均帧率从 63 掉到 46。FIFO_RELAXED 只在赶得上时才等，赶不上就立刻出图，正是给这种
    // 「快但不够稳」的帧时间准备的；撕裂在桌面窗口下由 DWM 合成，看不出来
    // Present mode (DM_PRESENT_MODE=fifo|relaxed|mailbox|immediate, default relaxed): with FIFO a frame that barely misses one period is pushed to the
    // next vsync, which in the town test compressed about 30% of frames into two periods and dragged the average from 63 down to 46. FIFO_RELAXED waits
    // only when it can make the deadline and presents immediately otherwise, exactly right for this "fast but not steady" frame time; tearing is hidden
    // by DWM composition in a desktop window
    VkResult pr = vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes;
    if (!pr && n) { modes.resize(n); pr = vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &n, modes.data()); }
    auto has_mode = [&](VkPresentModeKHR m) { for (auto x : modes) if (x == m) return true; return false; };
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    if (const char *e = getenv("DM_PRESENT_MODE")) {
        if (!strcmp(e, "fifo")) want = VK_PRESENT_MODE_FIFO_KHR;
        else if (!strcmp(e, "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (!strcmp(e, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (!strcmp(e, "relaxed")) want = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    if (!has_mode(want)) want = VK_PRESENT_MODE_FIFO_KHR;   // 不支持就退回 FIFO（唯一保证支持的）| fall back to FIFO, the only guaranteed mode
    ci.presentMode = want;
    ci.clipped = VK_TRUE; ci.oldSwapchain = swapchain;
    VkSwapchainKHR replacement = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(device, &ci, nullptr, &replacement);
    destroy_present_semaphores();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = r ? VK_NULL_HANDLE : replacement;
    images.clear();
    if (r) return r;
    swap_format = sf.format;
    vlog("Swapchain: client=%ux%u extent=%ux%u format=%d", cw, ch, extent.width, extent.height, int(swap_format));
    n = 0; r = vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr); if (r) return r;
    images.resize(n); r = vkGetSwapchainImagesKHR(device, swapchain, &n, images.data()); if (r) return r;
    images.resize(n);
    finished.resize(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto &s : finished) { r = vkCreateSemaphore(device, &sc, nullptr, &s); if (r) return r; }
    return VK_SUCCESS;
}
int has_window(void *) { return window ? 1 : 0; }

// present：源图（引擎交来，TRANSFER_SRC_OPTIMAL）→ 交换链（居中留边、按比例缩放到客户区）
// present: source image (handed over by the engine in TRANSFER_SRC_OPTIMAL) -> swapchain (centered with letterbox, scaled to the client area)
int present(void *, void *, VkImage source, VkFormat source_format, uint32_t width, uint32_t height, const int32_t crop[4], int32_t) {
    std::lock_guard<std::mutex> render_lock(render_mutex);
    if (!ready || !window) return VK_ERROR_INITIALIZATION_FAILED;
    VkFormatProperties sp;
    vkGetPhysicalDeviceFormatProperties(physical, source_format, &sp);
    if (!(sp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    int x = 0, y = 0, w = int(width), h = int(height);
    if (crop && crop[2] > 0 && crop[3] > 0) { x = std::clamp(crop[0], 0, w); y = std::clamp(crop[1], 0, h); w = std::min(crop[2], int(width) - x); h = std::min(crop[3], int(height) - y); }
    if (w <= 0 || h <= 0) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r;
    // 交换链重建：客户区尺寸变化（WM_SIZE 打脏）或驱动报出过期
    // Swapchain rebuild: the client area changed (WM_SIZE marks it dirty) or the driver reports out of date
    if (!surface || swap_dirty || built_generation != generation) {
        if ((r = vkQueueWaitIdle(queue))) return r;
        if (!surface) { r = VkResult(create_surface()); if (r) return r; }
        r = VkResult(create_swap());
        if (r) return r;
        swap_dirty = false; built_generation = generation;
    }
    VkFormatProperties dp;
    vkGetPhysicalDeviceFormatProperties(physical, swap_format, &dp);
    if (!(dp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) return VK_ERROR_FORMAT_NOT_SUPPORTED;
    // 两张在途帧交替：游戏可在 GPU 收尾本帧时翻译下一帧（同安卓 overlap）
    // Alternate two in-flight frames so the game can translate the next frame while the GPU finishes this one (as Android's overlap)
    bool overlap = overlap_pool != VK_NULL_HANDLE;
    unsigned f = overlap ? flight : 0u;
    VkCommandPool pool = f ? overlap_pool : command_pool;
    VkCommandBuffer cmd = f ? overlap_command : command;
    VkSemaphore acq = f ? overlap_acquired : acquired;
    VkFence done = f ? overlap_fence : fence;
    if (in_flight[f]) {
        r = vkWaitForFences(device, 1, &done, VK_TRUE, 10ull * 1000000000ull);
        if (r) return r;
        if ((r = vkResetFences(device, 1, &done))) return r;
        in_flight[f] = false;
    }
    uint32_t index = 0;
    r = vkAcquireNextImageKHR(device, swapchain, 50ull * 1000000ull, acq, VK_NULL_HANDLE, &index);
    if (r == VK_TIMEOUT || r == VK_NOT_READY) return VK_SUCCESS;   // 本帧不上屏：引擎把出图交给我们，这里不重试、不占队列 | this frame is not shown: the engine handed the picture to us, we do not retry or hold the queue
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swap_dirty = true; return VK_SUCCESS; }
    bool rebuild = r == VK_SUBOPTIMAL_KHR;
    if (r != VK_SUCCESS && !rebuild) return r;
    // DM_WIN_SHOT=<呈现序号>:<路径模板>：在第 N 次上屏时把引擎源图读回写成 BMP（路径里 %d 换成序号），供无窗口截图权限时核对画面
    // DM_WIN_SHOT=<present index>:<path template>: on the Nth present, read the engine's source image back and write it as a BMP (the %d in the path becomes the index), so the picture can be checked without screen-capture permission
    static int shot_at = -2, shot_step = 0; static std::string shot_path; static uint64_t presents;
    if (shot_at == -2) {
        shot_at = -1; const char *e = getenv("DM_WIN_SHOT");
        if (e && *e) { const char *c = strchr(e, ':'); if (c) { shot_at = atoi(e); if (e[0] && strchr(e, '+')) shot_step = atoi(strchr(e, '+') + 1); shot_path = c + 1; } }
    }
    ++presents;
    const bool want_shot = shot_at >= 0 && (presents == uint64_t(shot_at) || (shot_step > 0 && presents > uint64_t(shot_at) && (presents - uint64_t(shot_at)) % uint64_t(shot_step) == 0));
    VkBuffer shot_buf = VK_NULL_HANDLE; VkDeviceMemory shot_mem = VK_NULL_HANDLE; void *shot_map = nullptr;
    if (want_shot) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = size_t(width) * height * 4; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bci, nullptr, &shot_buf)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(device, shot_buf, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size;
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mai.memoryTypeIndex = i; break; }
        if (vkAllocateMemory(device, &mai, nullptr, &shot_mem) || vkBindBufferMemory(device, shot_buf, shot_mem, 0)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        vlog("DM_WIN_SHOT：第 %llu 次上屏读回源图 %ux%u", (unsigned long long)presents, width, height);
    }
    if ((r = vkResetCommandPool(device, pool, 0))) return r;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if ((r = vkBeginCommandBuffer(cmd, &bi))) return r;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.dstQueueFamilyIndex = barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[index];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    // 换布局放在等获取信号量的 TRANSFER 阶段之后（同安卓：否则可能在呈现引擎读完之前就写这张图） | the layout transition goes after the TRANSFER wait stage for the acquire semaphore (as Android: otherwise the image can be written before the presentation engine finishes reading it)
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    double scale = std::min(double(extent.width) / w, double(extent.height) / h);
    int dw = std::max(1, int(w * scale)), dh = std::max(1, int(h * scale));
    int dx = (int(extent.width) - dw) / 2, dy = (int(extent.height) - dh) / 2;
    if (dx || dy || dw != int(extent.width) || dh != int(extent.height)) {
        VkClearColorValue black{}; memset(&black, 0, sizeof black);
        vkCmdClearColorImage(cmd, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &barrier.subresourceRange);
        barrier.oldLayout = barrier.newLayout;
        barrier.srcAccessMask = barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[0] = {x, y, 0}; blit.srcOffsets[1] = {x + w, y + h, 1};
    blit.dstOffsets[0] = {dx, dy, 0}; blit.dstOffsets[1] = {dx + dw, dy + dh, 1};
    // 1:1 时最近邻与线性无差别；放大时（窗口比画面大、或高 DPI 缩放）有线性能力就用线性，斜边不至于发硬
    // At 1:1 nearest and linear are identical; when upscaling (window bigger than the picture, or high-DPI scaling) use linear when the format supports it, so diagonals do not look blocky
    VkFilter filter = VK_FILTER_NEAREST;
    if ((dw != w || dh != h) && (sp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) filter = VK_FILTER_LINEAR;
    vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
    if (shot_buf) {
        // 源图此时就是 TRANSFER_SRC_OPTIMAL：整张拷进宿主可见缓冲 | the source is already in TRANSFER_SRC_OPTIMAL here: copy the whole image into the host-visible buffer
        VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; region.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot_buf, 1, &region);
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; bb.buffer = shot_buf; bb.offset = 0; bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
    }
    VkMemoryBarrier source_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    source_read.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    source_read.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &source_read, 0, nullptr, 0, nullptr);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    if ((r = vkEndCommandBuffer(cmd))) return r;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acq; submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1; submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &finished[index];
    if ((r = vkQueueSubmit(queue, 1, &submit, done))) return r;
    in_flight[f] = true;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &finished[index];
    pi.swapchainCount = 1; pi.pSwapchains = &swapchain; pi.pImageIndices = &index;
    VkResult pr = vkQueuePresentKHR(queue, &pi);
    if (overlap) { flight ^= 1; if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR && (r = vkQueueWaitIdle(queue))) return r; }
    else {
        if ((r = vkWaitForFences(device, 1, &done, VK_TRUE, 10ull * 1000000000ull))) return r;
        if ((r = vkResetFences(device, 1, &done))) return r;
        in_flight[0] = false;
        if ((r = vkQueueWaitIdle(queue))) return r;
    }
    if (shot_buf) {
        if (vkQueueWaitIdle(queue) == VK_SUCCESS && vkMapMemory(device, shot_mem, 0, VK_WHOLE_SIZE, 0, &shot_map) == VK_SUCCESS && shot_map) {
            char name[1200]; snprintf(name, sizeof name, shot_path.c_str(), int(presents));
            FILE *o = fopen(name, "wb");
            if (o) {
                uint32_t hdr[11] = {0}; size_t stride = size_t(width) * 4, bytes = stride * height;
                uint8_t fh[14] = {'B', 'M'};
                uint32_t fsize = 54 + (uint32_t)bytes, off = 54;
                memcpy(fh + 2, &fsize, 4); memcpy(fh + 10, &off, 4);
                hdr[0] = 40; hdr[1] = width; hdr[2] = height; hdr[3] = 1u | (32u << 16); hdr[4] = 0; hdr[5] = (uint32_t)bytes;
                fwrite(fh, 1, 14, o); fwrite(hdr, 4, 10, o);
                // BMP 的 32 位像素是 BGRA；引擎源图的字节序按它的 Vulkan 格式来，RGBA 那些要先换红蓝，否则截出来的图红蓝相反
                // A 32-bit BMP stores BGRA; the engine's source image is in its Vulkan format's byte order, so the RGBA ones need red and blue
                // swapped on the way out, otherwise the shot comes out with red and blue reversed
                const bool swap_rb = source_format == VK_FORMAT_R8G8B8A8_UNORM || source_format == VK_FORMAT_R8G8B8A8_SRGB || source_format == VK_FORMAT_R8G8B8A8_SNORM;
                std::vector<uint8_t> row(stride);
                for (int y = height - 1; y >= 0; y--) {
                    const uint8_t *src = (const uint8_t *)shot_map + size_t(y) * stride;
                    if (swap_rb) { for (size_t i = 0; i + 3 < stride; i += 4) { row[i] = src[i + 2]; row[i + 1] = src[i + 1]; row[i + 2] = src[i]; row[i + 3] = src[i + 3]; } fwrite(row.data(), 1, stride, o); }
                    else fwrite(src, 1, stride, o);   // BMP 自下而上 | BMP is bottom-up
                }
                fclose(o);
                vlog("DM_WIN_SHOT：已写 %s（源格式 %d，交换红蓝 %d）", name, int(source_format), int(swap_rb));
            }
            vkUnmapMemory(device, shot_mem);
        }
        vkDestroyBuffer(device, shot_buf, nullptr); vkFreeMemory(device, shot_mem, nullptr);
        shot_buf = VK_NULL_HANDLE;
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR) { swap_dirty = true; return VK_SUCCESS; }    if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) return pr;
    if (rebuild || pr == VK_SUBOPTIMAL_KHR) swap_dirty = true;   // 下一帧重建 | rebuild on the next frame
    // 帧率统计：窗口 0.75 s 一次，取整后给标题栏 | frame rate: a 0.75 s window, rounded, handed to the title bar
    { uint64_t now = GetTickCount64();   // 毫秒（此前误按 2 ms 单位折算，标题栏的帧率报了实际的两倍）| milliseconds (it used to be treated as 2 ms units, which made the title show twice the real rate)
      if (!fps_window_start) fps_window_start = now;
      ++fps_window_frames;
      uint64_t span = now - fps_window_start;
      if (span >= 750) { fps_value.store(int((fps_window_frames * 1000ull + span / 2) / span)); fps_window_start = now; fps_window_frames = 0; } }
    return VK_SUCCESS;
}
}   // namespace

extern "C" int dm_win_vk_fps(void) { return fps_value.load(); }

extern "C" void dm_win_vk_notify_resize(void) {
    std::lock_guard<std::mutex> l(render_mutex);
    ++generation;
    swap_dirty = true;
}
extern "C" int dm_win_vk_has_window(void) { return has_window(nullptr); }
extern "C" const char *dm_win_vk_device_name(void) { return device_name.c_str(); }

extern "C" int dm_win_vk_initialize(int image_count, char *error, size_t bytes) {
    (void)image_count;
    auto fail = [&](const char *what, VkResult r) -> int {
        if (error && bytes) snprintf(error, bytes, "%s: VkResult %d", what, int(r));
        vlog("【红】%s: VkResult %d", what, int(r));
        return int(r ? r : VK_ERROR_INITIALIZATION_FAILED);
    };
    std::lock_guard<std::mutex> render_lock(render_mutex);
    if (ready) return 0;
    const char *dir = getenv("DM_SHADER_DIR");
    shader_dir = dir && *dir ? dir : "shaders";
    // 窗口由前端建好后才进来；没有窗口就不初始化（调用方会提示并按无呈现继续跑客体）
    // Called only after the front end has created the window; without a window we do not initialize (the caller then warns and keeps running the guest without presentation)
    extern HWND front_win_hwnd(void);
    window = front_win_hwnd();
    if (!window) return fail("No window", VK_ERROR_INITIALIZATION_FAILED);

    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    ai.pApplicationName = "dm-win"; ai.apiVersion = VK_API_VERSION_1_1;
    const char *ext[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &ai; ic.enabledExtensionCount = 2; ic.ppEnabledExtensionNames = ext;
    VkResult r = vkCreateInstance(&ic, nullptr, &instance);
    if (r) return fail("vkCreateInstance", r);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(instance, &n, devices.data());
    if (!n) return fail("No Vulkan device", VK_ERROR_INITIALIZATION_FAILED);
    // 先排序：非 CPU 设备优先，其次按设备类型（独显 > 集显 > 虚拟 > 其他）
    // Sort first: non-CPU devices first, then by device type (discrete > integrated > virtual > other)
    auto rank = [](VkPhysicalDevice d) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(d, &p);
        int t = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 0 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 4 : 3;
        return t;
    };
    std::stable_sort(devices.begin(), devices.end(), [&](VkPhysicalDevice a, VkPhysicalDevice b) { return rank(a) < rank(b); });
    physical = devices[0];
    auto try_dev = [&](VkPhysicalDevice d) -> int {
        physical = d;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> q(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &qn, q.data());
        // 队列族先随便选一个「图形 + 计算」的，表面建好之后再核对它能不能呈现，不行就换下一个族
        // Pick any "graphics + compute" family for now; after the surface exists, check it can present, otherwise try the next one
        uint32_t fam = UINT32_MAX;
        for (uint32_t i = 0; i < qn; i++) if ((q[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { fam = i; break; }
        if (fam == UINT32_MAX) return VK_ERROR_FEATURE_NOT_PRESENT;
        family = fam;
        if (create_surface()) { if (surface) { vkDestroySurfaceKHR(instance, surface, nullptr); surface = VK_NULL_HANDLE; } return VK_ERROR_FEATURE_NOT_PRESENT; }
        return 0;
    };
    int ok = 1;
    for (auto d : devices) if (!try_dev(d)) { ok = 0; break; }
    if (ok) return fail("No device with graphics/compute/presentable queue", VK_ERROR_FEATURE_NOT_PRESENT);
    VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &draw;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> avail(en);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &en, avail.data());
    bool mirror = false;
    for (auto &e : avail) if (!strcmp(e.extensionName, VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME)) mirror = true;
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(physical, &props);
    device_name = props.deviceName;
    vlog("Device %s api=%u.%u.%u driver=%#x type=%d", props.deviceName, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion, int(props.deviceType));
    std::vector<const char *> dext{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (mirror) dext.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    float priority = 1;
    VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = family; qc.queueCount = 1; qc.pQueuePriorities = &priority;
    VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.pNext = &draw; dc.pEnabledFeatures = &features.features;
    dc.queueCreateInfoCount = 1; dc.pQueueCreateInfos = &qc;
    dc.enabledExtensionCount = uint32_t(dext.size()); dc.ppEnabledExtensionNames = dext.data();
    if ((r = vkCreateDevice(physical, &dc, nullptr, &device))) return fail("vkCreateDevice", r);
    vkGetDeviceQueue(device, family, 0, &queue);
    destroy_swap();
    r = VkResult(create_surface());
    if (r) return fail("Win32 surface", r);
    if ((r = VkResult(create_swap()))) return fail("Swapchain", r);
    swap_dirty = false; built_generation = generation;
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cp.queueFamilyIndex = family;
    if ((r = vkCreateCommandPool(device, &cp, nullptr, &command_pool))) return fail("Present command pool", r);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = command_pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    if ((r = vkAllocateCommandBuffers(device, &ca, &command))) return fail("Present command", r);
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if ((r = vkCreateSemaphore(device, &sc, nullptr, &acquired))) return fail("Acquire semaphore", r);
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if ((r = vkCreateFence(device, &fc, nullptr, &fence))) return fail("Present fence", r);
    // 第二套（在途帧 1）：与安卓 overlap 同一取舍，默认开
    // The second set (in-flight frame 1): the same choice as Android's overlap, on by default
    bool overlap = true;
    if (const char *e = getenv("DM_PRESENT_OVERLAP")) overlap = *e != '0';
    if (overlap) {
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; cp.queueFamilyIndex = family;
        if ((r = vkCreateCommandPool(device, &cp, nullptr, &overlap_pool))) return fail("Present command pool 2", r);
        ca.commandPool = overlap_pool;
        if ((r = vkAllocateCommandBuffers(device, &ca, &overlap_command))) return fail("Present command 2", r);
        if ((r = vkCreateSemaphore(device, &sc, nullptr, &overlap_acquired))) return fail("Acquire semaphore 2", r);
        if ((r = vkCreateFence(device, &fc, nullptr, &overlap_fence))) return fail("Present fence 2", r);
    }
    vlog("Present overlap=%d (DM_PRESENT_OVERLAP)", int(overlap));

    dm_vk_config config{};
    config.size = sizeof(config); config.game.size = sizeof(config.game);
    config.game.physical = physical; config.game.device = device; config.game.queue = queue; config.game.queue_family = family;
    config.game.enabled_core = features.features;
    config.game.enabled_shader_draw_parameters = draw.shaderDrawParameters;
    config.game.fence_timeout_ns = 10ull * 1000000000ull;
    config.game.shader_directory = shader_dir.c_str();
    config.game.present = present;
    config.game.log = log_message;
    config.game.fatal = fatal;
    config.game.userdata = nullptr;
    config.sampler_mirror_clamp_to_edge = mirror;
    config.has_window = has_window;
    config.display_hz = 0;   // 由前端实测刷新率，不猜 60 | measured by the front end, do not guess 60
    int status = dm_vk_initialize(&config, error, bytes);
    if (!status) ready = true;
    else {
        ready = false;
        vlog("【红】dm_vk_initialize 失败：%s", error && bytes ? error : "(无)");
    }
    return status;
}
