/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Same-device LSFG-VK bridge for the Switch Vulkan renderer.
 *
 * The implementation is C++20, but this deliberately small C ABI keeps
 * Vita3K's Vulkan presentation integration independent of the vendored C++
 * backend. LSFG owns only its private images, pipelines and synchronization;
 * every Vita3K Vulkan handle is borrowed.
 */
#ifndef DOLPHIN_SWITCH_LSFG_BRIDGE_H
#define DOLPHIN_SWITCH_LSFG_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LsfgNxRuntime LsfgNxRuntime;

typedef struct LsfgNxCreateInfo {
  VkInstance instance;
  VkPhysicalDevice physical_device;
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family_index;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr;

  VkSwapchainKHR swapchain;
  VkExtent2D extent;
  const VkImage *swapchain_images;
  uint32_t swapchain_image_count;

  const char *shader_dll_path;
  float flow_scale;
  bool performance_mode;
} LsfgNxCreateInfo;

/* Optional profiling hook. Times are armGetSystemTick() values on Switch.
 * This never alters LSFG scheduling or synchronization. Stage values must
 * stay aligned with SwitchCpuProfilePhase in util/switch_thread.h. */
typedef enum LsfgNxDebugStage {
  LSFG_NX_DEBUG_SLOT_FENCE_WAIT = 0,
  LSFG_NX_DEBUG_RECORD_COMPUTE = 1,
  LSFG_NX_DEBUG_QUEUE_SUBMIT = 2,
  LSFG_NX_DEBUG_PRESENT_GENERATED = 3,
  LSFG_NX_DEBUG_ACQUIRE_IMAGE = 4,
  LSFG_NX_DEBUG_RESTORE_RECORD = 5,
  LSFG_NX_DEBUG_PRESENT_ORIGINAL = 6,
  LSFG_NX_DEBUG_CAPTURE_RECORD = 7
} LsfgNxDebugStage;
typedef void (*LsfgNxDebugCallback)(uint32_t stage, uint64_t start_tick,
                                     uint64_t end_tick);
void lsfg_nx_set_debug_callback(LsfgNxDebugCallback callback);

LsfgNxRuntime *lsfg_nx_create(const LsfgNxCreateInfo *info);
void lsfg_nx_destroy(LsfgNxRuntime *runtime);

/* Successful QueuePresentKHR calls, including both original and generated
 * images. The caller must serialize this query with present and destroy. */
uint64_t lsfg_nx_get_present_count(const LsfgNxRuntime *runtime);

/*
 * Returns true when the bridge consumed the presentation and wrote result.
 * Runtime faults are reported as errors. They deliberately do not fall back to
 * an ordinary presentation while the user has LSFG enabled.
 */
bool lsfg_nx_present(LsfgNxRuntime *runtime, VkQueue queue,
                     const VkPresentInfoKHR *present_info, VkResult *result);

#ifdef __cplusplus
}
#endif

#endif /* DOLPHIN_SWITCH_LSFG_BRIDGE_H */
