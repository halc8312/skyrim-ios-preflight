// Self-authored C-API fixture. No game data, shaders or extracted instructions.
#include "ProbeWebGPU.h"
#include "webgpu.h"
#include "wgpu.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

static_assert(sizeof(ProbeWebGPUReport) == 136);
static_assert(offsetof(ProbeWebGPUReport, rgba) == 72);
static_assert(sizeof(WGPUSurfaceSourceMetalLayer) == 24);
static_assert(offsetof(WGPUSurfaceSourceMetalLayer, layer) == 16);
static_assert(WGPUSType_SurfaceSourceMetalLayer == 4);

namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
std::mutex fixture_mutex;
// Process-lifetime callback storage is intentional: uncaptured errors can fire
// spontaneously from any thread, even when a failing probe is being torn down.
std::atomic<uint32_t> gpu_errors{0}, device_losses{0};

void uncaptured(WGPUDevice const *, WGPUErrorType, WGPUStringView, void *, void *) noexcept {
    gpu_errors.fetch_add(1, std::memory_order_relaxed);
}
void lost(WGPUDevice const *, WGPUDeviceLostReason reason, WGPUStringView, void *, void *) noexcept {
    if (reason != WGPUDeviceLostReason_Destroyed && reason != WGPUDeviceLostReason_CallbackCancelled)
        device_losses.fetch_add(1, std::memory_order_relaxed);
}

template<class T, void (*Release)(T)> struct Owned {
    T value = nullptr;
    Owned() = default;
    explicit Owned(T value) : value(value) {}
    Owned(const Owned &) = delete;
    Owned &operator=(const Owned &) = delete;
    ~Owned() { if (value) Release(value); }
};
using Texture = Owned<WGPUTexture, wgpuTextureRelease>;
using View = Owned<WGPUTextureView, wgpuTextureViewRelease>;
using Buffer = Owned<WGPUBuffer, wgpuBufferRelease>;
using Encoder = Owned<WGPUCommandEncoder, wgpuCommandEncoderRelease>;
using Command = Owned<WGPUCommandBuffer, wgpuCommandBufferRelease>;
using Pass = Owned<WGPURenderPassEncoder, wgpuRenderPassEncoderRelease>;

struct Session {
    WGPUInstance instance = nullptr;
    WGPUSurface surface = nullptr;
    WGPUAdapter adapter = nullptr;
    WGPUDevice device = nullptr;
    WGPUQueue queue = nullptr;
    bool configured = false;
    ~Session() {
        if (surface && configured) wgpuSurfaceUnconfigure(surface);
        if (queue) wgpuQueueRelease(queue);
        // Upstream may wait for driver completion here; not a preemptible API.
        if (device) wgpuDeviceRelease(device);
        if (adapter) wgpuAdapterRelease(adapter);
        if (surface) wgpuSurfaceRelease(surface);
        if (instance) wgpuInstanceRelease(instance);
    }
};

template<class T, void (*Release)(T)> struct Request {
    std::atomic<bool> done{false};
    uint32_t status = 0;
    T result = nullptr;
    ~Request() { if (result) Release(result); }
};
using AdapterRequest = Request<WGPUAdapter, wgpuAdapterRelease>;
using DeviceRequest = Request<WGPUDevice, wgpuDeviceRelease>;
struct MapRequest { std::atomic<bool> done{false}; uint32_t status = 0; };

template<class T> void *hold(const std::shared_ptr<T> &request) {
    return new std::shared_ptr<T>(request);
}
void adapter_ready(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView, void *user, void *) noexcept {
    std::unique_ptr<std::shared_ptr<AdapterRequest>> holder(static_cast<std::shared_ptr<AdapterRequest> *>(user));
    auto &state = **holder;
    state.result = adapter;
    state.status = status;
    state.done.store(true, std::memory_order_release);
}
void device_ready(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView, void *user, void *) noexcept {
    std::unique_ptr<std::shared_ptr<DeviceRequest>> holder(static_cast<std::shared_ptr<DeviceRequest> *>(user));
    auto &state = **holder;
    state.result = device;
    state.status = status;
    state.done.store(true, std::memory_order_release);
}
void map_ready(WGPUMapAsyncStatus status, WGPUStringView, void *user, void *) noexcept {
    std::unique_ptr<std::shared_ptr<MapRequest>> holder(static_cast<std::shared_ptr<MapRequest> *>(user));
    auto &state = **holder;
    state.status = status;
    state.done.store(true, std::memory_order_release);
}
template<class T> bool await(Session &session, const std::shared_ptr<T> &request, Deadline deadline) {
    while (!request->done.load(std::memory_order_acquire)) {
        if (Clock::now() >= deadline) return false;
        wgpuInstanceProcessEvents(session.instance);
        if (session.device) wgpuDevicePoll(session.device, WGPU_FALSE, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return Clock::now() < deadline;
}

int32_t fail(ProbeWebGPUReport &report, unsigned bit) {
    report.failed |= UINT64_C(1) << bit;
    return -10 - static_cast<int32_t>(bit);
}
void pass(ProbeWebGPUReport &report, unsigned bit) { report.passed |= UINT64_C(1) << bit; }

bool clear(Encoder &encoder, WGPUTextureView view, WGPUColor color) {
    WGPURenderPassColorAttachment attachment = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    attachment.view = view;
    attachment.loadOp = WGPULoadOp_Clear;
    attachment.storeOp = WGPUStoreOp_Store;
    attachment.clearValue = color;
    WGPURenderPassDescriptor descriptor = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    descriptor.colorAttachmentCount = 1;
    descriptor.colorAttachments = &attachment;
    Pass render(wgpuCommandEncoderBeginRenderPass(encoder.value, &descriptor));
    if (!render.value) return false;
    wgpuRenderPassEncoderEnd(render.value);
    return true;
}

int32_t run(void *layer, uint32_t width, uint32_t height, ProbeWebGPUReport &report, Deadline deadline) {
    report.native_version = wgpuGetVersion();
    if (report.native_version != 0x1D000101) return fail(report, 0);
    pass(report, 0);
    Session session;
    WGPUInstanceDescriptor instance_descriptor = WGPU_INSTANCE_DESCRIPTOR_INIT;
    session.instance = wgpuCreateInstance(&instance_descriptor);
    if (!session.instance) return fail(report, 1);
    pass(report, 1);
    WGPUSurfaceSourceMetalLayer metal = WGPU_SURFACE_SOURCE_METAL_LAYER_INIT;
    metal.layer = layer;
    WGPUSurfaceDescriptor surface_descriptor = WGPU_SURFACE_DESCRIPTOR_INIT;
    surface_descriptor.nextInChain = &metal.chain;
    session.surface = wgpuInstanceCreateSurface(session.instance, &surface_descriptor);
    if (!session.surface) return fail(report, 2);
    pass(report, 2);

    auto adapter = std::make_shared<AdapterRequest>();
    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.backendType = WGPUBackendType_Metal;
    options.compatibleSurface = session.surface;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    options.forceFallbackAdapter = WGPU_FALSE;
    WGPURequestAdapterCallbackInfo adapter_callback = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    adapter_callback.mode = WGPUCallbackMode_AllowProcessEvents;
    adapter_callback.callback = adapter_ready;
    adapter_callback.userdata1 = hold(adapter);
    wgpuInstanceRequestAdapter(session.instance, &options, adapter_callback);
    if (!await(session, adapter, deadline)) return -19;
    report.adapter_status = adapter->status;
    if (adapter->status != WGPURequestAdapterStatus_Success || !adapter->result) return fail(report, 3);
    session.adapter = std::exchange(adapter->result, nullptr);
    WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
    const WGPUStatus info_status = wgpuAdapterGetInfo(session.adapter, &info);
    report.backend_type = info.backendType;
    report.adapter_type = info.adapterType;
    wgpuAdapterInfoFreeMembers(info);
    if (info_status != WGPUStatus_Success || report.backend_type != WGPUBackendType_Metal
        || report.adapter_type == WGPUAdapterType_CPU) return fail(report, 3);
    pass(report, 3);

    auto device = std::make_shared<DeviceRequest>();
    WGPUDeviceDescriptor device_descriptor = WGPU_DEVICE_DESCRIPTOR_INIT;
    device_descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowProcessEvents;
    device_descriptor.deviceLostCallbackInfo.callback = lost;
    device_descriptor.uncapturedErrorCallbackInfo.callback = uncaptured;
    WGPURequestDeviceCallbackInfo device_callback = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    device_callback.mode = WGPUCallbackMode_AllowProcessEvents;
    device_callback.callback = device_ready;
    device_callback.userdata1 = hold(device);
    wgpuAdapterRequestDevice(session.adapter, &device_descriptor, device_callback);
    if (!await(session, device, deadline)) return -19;
    report.device_status = device->status;
    if (device->status != WGPURequestDeviceStatus_Success || !device->result) return fail(report, 4);
    session.device = std::exchange(device->result, nullptr);
    session.queue = wgpuDeviceGetQueue(session.device);
    if (!session.queue) return fail(report, 4);
    pass(report, 4);

    WGPUSurfaceCapabilities caps = WGPU_SURFACE_CAPABILITIES_INIT;
    const WGPUStatus caps_status = wgpuSurfaceGetCapabilities(session.surface, session.adapter, &caps);
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    WGPUCompositeAlphaMode alpha = WGPUCompositeAlphaMode_Auto;
    bool fifo = false;
    for (size_t i = 0; i < caps.formatCount; ++i)
        if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm || caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
            format = caps.formats[i]; break;
        }
    for (size_t i = 0; i < caps.presentModeCount; ++i) fifo |= caps.presentModes[i] == WGPUPresentMode_Fifo;
    if (caps.alphaModeCount) alpha = caps.alphaModes[0];
    const bool usable = caps_status == WGPUStatus_Success && format != WGPUTextureFormat_Undefined
        && fifo && caps.alphaModeCount && (caps.usages & WGPUTextureUsage_RenderAttachment);
    wgpuSurfaceCapabilitiesFreeMembers(caps);
    if (!usable) return fail(report, 5);
    WGPUSurfaceConfiguration configuration = WGPU_SURFACE_CONFIGURATION_INIT;
    configuration.device = session.device;
    configuration.format = format;
    configuration.usage = WGPUTextureUsage_RenderAttachment;
    configuration.width = width;
    configuration.height = height;
    configuration.alphaMode = alpha;
    configuration.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(session.surface, &configuration);
    session.configured = true;
    pass(report, 5);

    WGPUSurfaceTexture current = WGPU_SURFACE_TEXTURE_INIT;
    if (Clock::now() >= deadline) return -19;
    wgpuSurfaceGetCurrentTexture(session.surface, &current);
    report.surface_status = current.status;
    Texture frame(current.texture);
    if ((current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal
        && current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) || !frame.value)
        return fail(report, 6);
    View frame_view(wgpuTextureCreateView(frame.value, nullptr));
    WGPUCommandEncoderDescriptor encoder_descriptor = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    Encoder surface_encoder(wgpuDeviceCreateCommandEncoder(session.device, &encoder_descriptor));
    if (!frame_view.value || !surface_encoder.value || !clear(surface_encoder, frame_view.value, {0, 1, 0, 1}))
        return fail(report, 6);
    WGPUCommandBufferDescriptor command_descriptor = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    Command surface_commands(wgpuCommandEncoderFinish(surface_encoder.value, &command_descriptor));
    if (!surface_commands.value) return fail(report, 6);
    wgpuQueueSubmit(session.queue, 1, &surface_commands.value);
    if (wgpuSurfacePresent(session.surface) != WGPUStatus_Success) return fail(report, 6);
    pass(report, 6);

    WGPUTextureDescriptor texture_descriptor = WGPU_TEXTURE_DESCRIPTOR_INIT;
    texture_descriptor.size = {4, 4, 1};
    texture_descriptor.dimension = WGPUTextureDimension_2D;
    texture_descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    texture_descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    Texture texture(wgpuDeviceCreateTexture(session.device, &texture_descriptor));
    if (!texture.value) return fail(report, 7);
    View view(wgpuTextureCreateView(texture.value, nullptr));
    WGPUBufferDescriptor buffer_descriptor = WGPU_BUFFER_DESCRIPTOR_INIT;
    buffer_descriptor.size = 4 * 256;
    buffer_descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    Buffer buffer(wgpuDeviceCreateBuffer(session.device, &buffer_descriptor));
    Encoder encoder(wgpuDeviceCreateCommandEncoder(session.device, &encoder_descriptor));
    if (!view.value || !buffer.value || !encoder.value || !clear(encoder, view.value, {1, 0, 0, 1})) return fail(report, 7);
    WGPUTexelCopyTextureInfo source = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    source.texture = texture.value;
    source.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo destination = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    destination.buffer = buffer.value;
    destination.layout.bytesPerRow = 256;
    destination.layout.rowsPerImage = 4;
    WGPUExtent3D extent{4, 4, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder.value, &source, &destination, &extent);
    Command commands(wgpuCommandEncoderFinish(encoder.value, &command_descriptor));
    if (!commands.value) return fail(report, 7);
    wgpuQueueSubmit(session.queue, 1, &commands.value);
    pass(report, 7);

    auto mapped = std::make_shared<MapRequest>();
    WGPUBufferMapCallbackInfo map_callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    map_callback.mode = WGPUCallbackMode_AllowProcessEvents;
    map_callback.callback = map_ready;
    map_callback.userdata1 = hold(mapped);
    wgpuBufferMapAsync(buffer.value, WGPUMapMode_Read, 0, 1024, map_callback);
    if (!await(session, mapped, deadline)) return -19;
    report.map_status = mapped->status;
    if (mapped->status != WGPUMapAsyncStatus_Success) return fail(report, 8);
    const auto *bytes = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(buffer.value, 0, 1024));
    bool correct = bytes != nullptr;
    if (bytes) for (size_t y = 0; y < 4; ++y) for (size_t x = 0; x < 4; ++x) {
        const auto *pixel = bytes + y * 256 + x * 4;
        std::memcpy(report.rgba + (y * 4 + x) * 4, pixel, 4);
        correct &= pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
        ++report.pixel_count;
    }
    wgpuBufferUnmap(buffer.value);
    if (!correct) return fail(report, 8);
    pass(report, 8);
    return 0;
}
} // namespace

extern "C" int32_t probe_webgpu_metal(void *layer, uint32_t width, uint32_t height,
                                      ProbeWebGPUReport *out, size_t capacity) {
    if (!out) return -1;
    if (capacity < sizeof(*out)) return -2;
    if (reinterpret_cast<uintptr_t>(out) % alignof(ProbeWebGPUReport)) return -4;
    std::unique_lock lock(fixture_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return -5;
    ProbeWebGPUReport report{};
    report.abi_version = 1;
    report.struct_size = sizeof(report);
    if (!layer || !width || !height || width > 4096 || height > 4096) {
        std::memcpy(out, &report, sizeof(report));
        return -3;
    }
    gpu_errors.store(0, std::memory_order_relaxed);
    device_losses.store(0, std::memory_order_relaxed);
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds(5);
    int32_t status;
    try { status = run(layer, width, height, report, deadline); }
    catch (...) { status = -21; }
    report.gpu_error_count = gpu_errors.load(std::memory_order_relaxed);
    report.device_lost_count = device_losses.load(std::memory_order_relaxed);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
    report.elapsed_ms = elapsed < UINT32_MAX ? static_cast<uint32_t>(elapsed) : UINT32_MAX;
    if (!report.gpu_error_count && !report.device_lost_count && status == 0) pass(report, 9);
    else if (report.gpu_error_count || report.device_lost_count) report.failed |= UINT64_C(1) << 9;
    if (Clock::now() < deadline && status == 0) pass(report, 10);
    else if (Clock::now() >= deadline) report.failed |= UINT64_C(1) << 10;
    if (status == 0 && (report.passed != PROBE_WEBGPU_EXPECTED_FLAGS || report.failed)) status = -20;
    std::memcpy(out, &report, sizeof(report));
    return status;
}
