// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 clarification 1, item 6: a geometry-shader draw and a transform-feedback capture, as
// one module both test programs use: vktest.exe on the device (through Wine) and the macOS CI's
// render test (tests/macos/, MoltenVK linked directly). Offscreen only: a 32 x 32 RGBA8 image and
// host-visible buffers, read back and compared. Never run on the owner's PC (no GPU tests there).
//
// The caller includes <vulkan/vulkan.h> with VK_NO_PROTOTYPES first, asks vkgx_query what the
// device offers, creates its device with what vkgx_device_chain adds, then calls vkgx_run.

#ifndef VKGX_H
#define VKGX_H

#include <stdint.h>

enum
{
    VKGX_FAIL_GEOMETRY_DRAW = 1 << 0,   /* the geometry shader's quad isn't where it should be */
    VKGX_FAIL_XFB           = 1 << 1,   /* the captured vertices or the byte count are wrong */
};

struct vkgx_caps
{
    int geometry;   /* geometryShader */
    int xfb;        /* VK_EXT_transform_feedback with transformFeedback (needs geometry here too:
                     * the fork captures transform feedback from geometry shaders only) */
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb_features;   /* what vkgx_device_chain enables */
};

struct vkgx_env
{
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;     /* created with geometryShader and, where caps.xfb, the extension */
    VkQueue queue;
    uint32_t family;     /* of queue: graphics */
};

/* what the physical device offers; prints one "geometry/xfb:" line */
void vkgx_query( PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
                 struct vkgx_caps *caps );

/* for the caller's VkDeviceCreateInfo: sets features->geometryShader when offered, and when
 * transform feedback is offered links caps->xfb_features in front of *next and returns the
 * extension name to enable (NULL otherwise) */
const char *vkgx_device_chain( struct vkgx_caps *caps, VkPhysicalDeviceFeatures *features, const void **next );

/* runs the tests caps allows, prints PASS/FAIL/SKIP lines, returns VKGX_FAIL_* bits */
unsigned int vkgx_run( const struct vkgx_env *env, const struct vkgx_caps *caps );

#endif
