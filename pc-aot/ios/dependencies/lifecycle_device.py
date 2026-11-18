"""D3D9 producer flush on a worker; native MoltenVK submission is unchanged."""


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Lifecycle DXVK anchor changed')
    return text.replace(old, new, 1)


def adapt(text):
    text = '#include "lifecycle_runtime.h"\n' + text
    text = once(text, 'namespace dxvk {', '''namespace dxvk {

  static int32_t iosLifecycleDrain(void* opaque, unsigned phase) {
    auto* device = static_cast<D3D9DeviceEx*>(opaque);
    try {
      if (phase == 0) {
        // Flush initializer and format converter before the threaded context.
        device->Flush();
      } else if (phase == 1) {
        device->SynchronizeCsThread();
      } else if (phase == 2) {
        auto dxvk = device->GetDXVKDevice();
        // lockSubmission first drains the submit queue. No native producer is
        // closed at this point; all admitted frontend work was flushed above.
        dxvk->lockSubmission();
        struct UnlockSubmission {
          decltype(dxvk)& device;
          ~UnlockSubmission() { device->unlockSubmission(); }
        } unlock { dxvk };
        auto vkd = dxvk->vkd();
        VkResult result = vkd->vkDeviceWaitIdle(vkd->device());
        return int32_t(result);
      } else return int32_t(VK_ERROR_UNKNOWN);
      return int32_t(VK_SUCCESS);
    } catch (const DxvkError& error) {
      Logger::err(error.message());
      return int32_t(VK_ERROR_UNKNOWN);
    }
  }
''')
    text = once(text, '    m_availableMemory = DetermineInitialTextureMemory();',
        '    m_availableMemory = DetermineInitialTextureMemory();\n'
        '    skyrim_lifecycle_register_device(this, iosLifecycleDrain);')
    return once(text, '  D3D9DeviceEx::~D3D9DeviceEx() {',
        '  D3D9DeviceEx::~D3D9DeviceEx() {\n'
        '    skyrim_lifecycle_unregister_device(this);')
