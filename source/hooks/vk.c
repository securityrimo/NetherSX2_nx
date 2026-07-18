/* vk.c -- Vulkan (Mesa NVK) bridge. See vk.h for the why.
 *
 * Three functional shims sit between the core and NVK:
 *   1. vkCreateAndroidSurfaceKHR -> NVK's VI surface (the core has no VI path).
 *   2. vkQueuePresentKHR         -> count presented frames (main.c's liveness /
 *                                   CPU-boost-off trigger; the GL swap counter
 *                                   never moves on the Vulkan path).
 *   3. vkGetPhysicalDeviceMemoryProperties2 -> report the full heap as the
 *                                   VK_EXT_memory_budget budget. NVK reports the
 *                                   free-system-memory figure, which our newlib
 *                                   heap has eaten; VMA then believes it is over
 *                                   budget and refuses EVERY allocation without
 *                                   calling Vulkan -> the GS spins in bad_alloc
 *                                   and the screen stays black. This is the fix.
 * Plus vkCreateInstance / vkEnumerateInstanceExtensionProperties, which swap the
 * core's VK_KHR_android_surface for NVK's real VK_NN_vi_surface.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "vk.h"

#ifdef USE_VULKAN

#include <string.h>
#include <stdlib.h>
#include "../util.h"

volatile int vk_present_count = 0;

// surface: Android create-info -> NVK VI surface. ci->window is the core's
// ANativeWindow*, which our ANativeWindow_fromSurface_fake made == a libnx
// NWindow*, so it passes straight through to vkCreateViSurfaceNN.
static VkResult VKAPI_CALL
vkCreateAndroidSurfaceKHR_shim(VkInstance inst,
                               const VkAndroidSurfaceCreateInfoKHR *ci,
                               const VkAllocationCallbacks *alloc,
                               VkSurfaceKHR *out) {
  VkViSurfaceCreateInfoNN vi = {
      .sType  = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN,
      .pNext  = NULL,
      .flags  = 0,
      .window = (void *)ci->window, // NWindow* -- no conversion
  };
  VkResult r = vkCreateViSurfaceNN(inst, &vi, alloc, out);

  return r;
}

// present counter (main.c reads vk_present_count).
static PFN_vkQueuePresentKHR real_qpresent = NULL;
static VkResult VKAPI_CALL
vkQueuePresentKHR_shim(VkQueue q, const VkPresentInfoKHR *pi) {
  ++vk_present_count;
  return real_qpresent ? real_qpresent(q, pi) : vkQueuePresentKHR(q, pi);
}

// memory-budget override (the black-screen fix -- see file header).
static PFN_vkGetPhysicalDeviceMemoryProperties2 real_memprops2 = NULL;
static void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties2_shim(VkPhysicalDevice pd,
                                          VkPhysicalDeviceMemoryProperties2 *props) {
  if (!real_memprops2) return;
  real_memprops2(pd, props);
  uint32_t heaps = props->memoryProperties.memoryHeapCount;
  for (VkBaseOutStructure *p = (VkBaseOutStructure *)props->pNext; p; p = p->pNext) {
    if (p->sType != VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
      continue;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = (void *)p;
    for (uint32_t i = 0; i < heaps && i < VK_MAX_MEMORY_HEAPS; i++) {
      VkDeviceSize heap = props->memoryProperties.memoryHeaps[i].size;
      if (b->heapBudget[i] < heap) b->heapBudget[i] = heap;
    }
  }
}

// device proc addr wrapper: swap in our present shim; forward everything else to
// NVK's real GDPA unchanged.
static PFN_vkVoidFunction VKAPI_CALL
vk_gdpa_hook(VkDevice dev, const char *name) {
  PFN_vkVoidFunction fn = vkGetDeviceProcAddr(dev, name);
  if (!name) return fn;
  if (!strcmp(name, "vkQueuePresentKHR")) {
    real_qpresent = (PFN_vkQueuePresentKHR)fn;
    return (PFN_vkVoidFunction)vkQueuePresentKHR_shim;
  }
  return fn;
}

// instance proc addr wrapper -- registered in the import table as
// "vkGetInstanceProcAddr". Routes the Android surface, device-proc-addr, present
// and the memory-budget override; everything else is NVK's real GIPA.
PFN_vkVoidFunction VKAPI_CALL
vk_gipa_hook(VkInstance inst, const char *name) {
  if (name) {
    if (!strcmp(name, "vkCreateAndroidSurfaceKHR"))
      return (PFN_vkVoidFunction)vkCreateAndroidSurfaceKHR_shim;
    if (!strcmp(name, "vkGetDeviceProcAddr"))
      return (PFN_vkVoidFunction)vk_gdpa_hook;
    if (!strcmp(name, "vkQueuePresentKHR"))
      return (PFN_vkVoidFunction)vkQueuePresentKHR_shim;
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") ||
        !strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR")) {
      PFN_vkVoidFunction f = vkGetInstanceProcAddr(inst, name);
      if (!f) return NULL; // don't advertise what NVK doesn't have
      real_memprops2 = (PFN_vkGetPhysicalDeviceMemoryProperties2)f;
      return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2_shim;
    }
  }
  return vkGetInstanceProcAddr(inst, name);
}

// enumerate instance extensions -- advertise VK_KHR_android_surface (which NVK
// does NOT provide) so the core's availability check passes. Swapped for the real
// VK_NN_vi_surface at vkCreateInstance below.
VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties_hook(const char *layer, uint32_t *pCount,
                                            VkExtensionProperties *pProps) {
  if (pProps == NULL) {
    VkResult r = vkEnumerateInstanceExtensionProperties(layer, pCount, NULL);
    if (r == VK_SUCCESS) (*pCount)++; // reserve a slot for the injected name
    return r;
  }
  uint32_t want = *pCount, got = want ? want - 1 : 0;
  VkResult r = vkEnumerateInstanceExtensionProperties(layer, &got, pProps);
  if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;
  VkExtensionProperties inj = { VK_KHR_ANDROID_SURFACE_EXTENSION_NAME, 6 };
  if (got < want) { pProps[got++] = inj; *pCount = got; return VK_SUCCESS; }
  *pCount = got;
  return VK_INCOMPLETE;
}

// create instance -- swap the core's VK_KHR_android_surface for the real
// VK_NN_vi_surface so NVK accepts the instance and owns the VI surface extension.
VkResult VKAPI_CALL
vkCreateInstance_hook(const VkInstanceCreateInfo *ci,
                      const VkAllocationCallbacks *alloc, VkInstance *out) {
  uint32_t n = ci->enabledExtensionCount;
  const char **ext = malloc(sizeof(char *) * (n ? n : 1));
  for (uint32_t i = 0; i < n; i++) {
    ext[i] = strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_ANDROID_SURFACE_EXTENSION_NAME)
                 ? ci->ppEnabledExtensionNames[i]
                 : VK_NN_VI_SURFACE_EXTENSION_NAME;
  }
  VkInstanceCreateInfo m = *ci;
  m.ppEnabledExtensionNames = ext;
  VkResult r = vkCreateInstance(&m, alloc, out);
  free((void *)ext);

  return r;
}

#endif // USE_VULKAN
