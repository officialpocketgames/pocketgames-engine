// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 clarification 1, item 6: the geometry-shader draw and the transform-feedback capture
// (vkgx.h). Both draw into a 32 x 32 RGBA8 image cleared to blue:
//   - geometry: the vertex shader gives a tiny triangle at the centre; the geometry shader turns
//     it into a green quad from -0.5 to 0.5 (four vertices, two triangles). Pixels inside the
//     quad but far outside the tiny triangle must be green, pixels outside the quad blue.
//   - transform feedback: the vertex shader gives each of three vertices (i, 2i, 3i, 1); the
//     geometry shader passes the triangle on and captures each value plus (100, 0, 0, 0) into
//     buffer 0 (16 bytes a vertex). The buffer must hold the three values in order, nothing past
//     them, and the counter buffer 48 bytes.
// Shaders: shaders/*.vert, *.geom, *.frag, compiled to vkgx_shaders.h by gen-vkgx-shaders.sh.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include "vkgx.h"
#include "vkgx_shaders.h"

#define SIZE 32

#define DEVICE_FUNCS(X) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkMapMemory) X(vkUnmapMemory) X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) \
    X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdDraw) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyImageToBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle)
#define XFB_FUNCS(X) \
    X(vkCmdBindTransformFeedbackBuffersEXT) X(vkCmdBeginTransformFeedbackEXT) X(vkCmdEndTransformFeedbackEXT)

#define X(name) static PFN_##name p_##name;
DEVICE_FUNCS(X) XFB_FUNCS(X)
#undef X

static VkDevice device;
static VkQueue queue;
static VkCommandPool pool;
static VkPhysicalDeviceMemoryProperties memprops;

/* ---------------------------------------------------------------- caps */

static PFN_vkVoidFunction instance_fn( PFN_vkGetInstanceProcAddr gipa, VkInstance instance, const char *name,
                                       const char *khr_name )
{
    PFN_vkVoidFunction fn = gipa( instance, name );
    if (!fn && khr_name) fn = gipa( instance, khr_name );
    return fn;
}

void vkgx_query( PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice physical,
                 struct vkgx_caps *caps )
{
    PFN_vkGetPhysicalDeviceFeatures get = (PFN_vkGetPhysicalDeviceFeatures)instance_fn( gipa, instance,
        "vkGetPhysicalDeviceFeatures", NULL );
    PFN_vkGetPhysicalDeviceFeatures2 get2 = (PFN_vkGetPhysicalDeviceFeatures2)instance_fn( gipa, instance,
        "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR" );
    PFN_vkEnumerateDeviceExtensionProperties enum_ext = (PFN_vkEnumerateDeviceExtensionProperties)instance_fn(
        gipa, instance, "vkEnumerateDeviceExtensionProperties", NULL );
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &xfb };
    VkPhysicalDeviceFeatures f;
    VkExtensionProperties *exts;
    uint32_t n = 0, i;
    int offered = 0;

    memset( caps, 0, sizeof(*caps) );
    caps->xfb_features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
    if (get)
    {
        memset( &f, 0, sizeof(f) );
        get( physical, &f );
        caps->geometry = f.geometryShader;
    }
    if (enum_ext && enum_ext( physical, NULL, &n, NULL ) == VK_SUCCESS && n && (exts = calloc( n, sizeof(*exts) )))
    {
        if (enum_ext( physical, NULL, &n, exts ) >= 0)
            for (i = 0; i < n; i++) if (!strcmp( exts[i].extensionName, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME )) offered = 1;
        free( exts );
    }
    if (offered && get2)
    {
        get2( physical, &f2 );
        caps->xfb = caps->geometry && xfb.transformFeedback;
    }
    caps->xfb_features.transformFeedback = caps->xfb ? VK_TRUE : VK_FALSE;
    printf( "geometry/xfb: geometryShader %s, VK_EXT_transform_feedback %s, transformFeedback %s\n",
            caps->geometry ? "yes" : "no", offered ? "offered" : "not offered", xfb.transformFeedback ? "yes" : "no" );
}

const char *vkgx_device_chain( struct vkgx_caps *caps, VkPhysicalDeviceFeatures *features, const void **next )
{
    if (caps->geometry) features->geometryShader = VK_TRUE;
    if (!caps->xfb) return NULL;
    caps->xfb_features.pNext = (void *)*next;
    *next = &caps->xfb_features;
    return VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME;
}

/* ---------------------------------------------------------------- helpers */

static uint32_t memory_type( uint32_t bits, VkMemoryPropertyFlags want )
{
    uint32_t i;
    for (i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

struct buffer { VkBuffer buffer; VkDeviceMemory memory; void *map; };

/* a host-visible, coherent, mapped buffer */
static int make_buffer( VkDeviceSize size, VkBufferUsageFlags usage, struct buffer *b )
{
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    memset( b, 0, sizeof(*b) );
    if (p_vkCreateBuffer( device, &info, NULL, &b->buffer ) != VK_SUCCESS) return 0;
    p_vkGetBufferMemoryRequirements( device, b->buffer, &req );
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type( req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT );
    if (alloc.memoryTypeIndex == UINT32_MAX) return 0;
    if (p_vkAllocateMemory( device, &alloc, NULL, &b->memory ) != VK_SUCCESS) return 0;
    if (p_vkBindBufferMemory( device, b->buffer, b->memory, 0 ) != VK_SUCCESS) return 0;
    return p_vkMapMemory( device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->map ) == VK_SUCCESS;
}

static void free_buffer( struct buffer *b )
{
    if (b->map) p_vkUnmapMemory( device, b->memory );
    if (b->buffer) p_vkDestroyBuffer( device, b->buffer, NULL );
    if (b->memory) p_vkFreeMemory( device, b->memory, NULL );
    memset( b, 0, sizeof(*b) );
}

/* the image the tests draw into, its render pass (clear, store, then ready to copy) and framebuffer */
struct target { VkImage image; VkDeviceMemory memory; VkImageView view; VkRenderPass pass; VkFramebuffer fb; };

static int make_target( struct target *t )
{
    VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { SIZE, SIZE, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageViewCreateInfo vinfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref };
    /* the copy after the pass waits for the attachment's writes */
    VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
    VkRenderPassCreateInfo rp = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
        .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub, .dependencyCount = 1, .pDependencies = &dep };
    VkFramebufferCreateInfo fb = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .attachmentCount = 1,
        .width = SIZE, .height = SIZE, .layers = 1 };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements req;

    memset( t, 0, sizeof(*t) );
    if (p_vkCreateImage( device, &info, NULL, &t->image ) != VK_SUCCESS) return 0;
    p_vkGetImageMemoryRequirements( device, t->image, &req );
    alloc.allocationSize = req.size;
    if ((alloc.memoryTypeIndex = memory_type( req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT )) == UINT32_MAX) return 0;
    if (p_vkAllocateMemory( device, &alloc, NULL, &t->memory ) != VK_SUCCESS) return 0;
    if (p_vkBindImageMemory( device, t->image, t->memory, 0 ) != VK_SUCCESS) return 0;
    vinfo.image = t->image;
    if (p_vkCreateImageView( device, &vinfo, NULL, &t->view ) != VK_SUCCESS) return 0;
    if (p_vkCreateRenderPass( device, &rp, NULL, &t->pass ) != VK_SUCCESS) return 0;
    fb.renderPass = t->pass;
    fb.pAttachments = &t->view;
    return p_vkCreateFramebuffer( device, &fb, NULL, &t->fb ) == VK_SUCCESS;
}

static void free_target( struct target *t )
{
    if (t->fb) p_vkDestroyFramebuffer( device, t->fb, NULL );
    if (t->pass) p_vkDestroyRenderPass( device, t->pass, NULL );
    if (t->view) p_vkDestroyImageView( device, t->view, NULL );
    if (t->image) p_vkDestroyImage( device, t->image, NULL );
    if (t->memory) p_vkFreeMemory( device, t->memory, NULL );
    memset( t, 0, sizeof(*t) );
}

static VkShaderModule shader( const uint32_t *code, size_t size )
{
    VkShaderModuleCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule module = VK_NULL_HANDLE;
    if (p_vkCreateShaderModule( device, &info, NULL, &module ) != VK_SUCCESS) return VK_NULL_HANDLE;
    return module;
}

/* vertex, geometry and fragment stages, a triangle list with no vertex buffer, no culling */
static VkPipeline make_pipeline( const struct target *t, VkPipelineLayout layout, const uint32_t *vs, size_t vs_size,
                                 const uint32_t *gs, size_t gs_size )
{
    VkShaderModule modules[3] = { shader( vs, vs_size ), shader( gs, gs_size ), shader( gs_color_frag, sizeof(gs_color_frag) ) };
    VkPipelineShaderStageCreateInfo stages[3] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_GEOMETRY_BIT, .module = modules[1], .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[2], .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport viewport = { 0, 0, SIZE, SIZE, 0, 1 };
    VkRect2D scissor = { { 0, 0 }, { SIZE, SIZE } };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState blend = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &blend };
    VkGraphicsPipelineCreateInfo info = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 3,
        .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = layout,
        .renderPass = t->pass, .subpass = 0 };
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    int i;

    if (modules[0] && modules[1] && modules[2])
        res = p_vkCreateGraphicsPipelines( device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline );
    if (res != VK_SUCCESS) printf( "vkgx: vkCreateGraphicsPipelines: %d\n", (int)res );
    for (i = 0; i < 3; i++) if (modules[i]) p_vkDestroyShaderModule( device, modules[i], NULL );
    return res == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
}

static VkCommandBuffer begin( void )
{
    VkCommandBufferAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkCommandBuffer cb = VK_NULL_HANDLE;

    if (p_vkAllocateCommandBuffers( device, &info, &cb ) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (p_vkBeginCommandBuffer( cb, &begin_info ) != VK_SUCCESS) return VK_NULL_HANDLE;
    return cb;
}

/* ends, submits and waits; the command buffer goes with the pool */
static int submit( VkCommandBuffer cb )
{
    VkSubmitInfo info = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    VkResult res;

    if ((res = p_vkEndCommandBuffer( cb )) == VK_SUCCESS &&
        (res = p_vkQueueSubmit( queue, 1, &info, VK_NULL_HANDLE )) == VK_SUCCESS)
        res = p_vkQueueWaitIdle( queue );
    if (res != VK_SUCCESS) printf( "vkgx: submit: %d\n", (int)res );
    return res == VK_SUCCESS;
}

static void begin_pass( VkCommandBuffer cb, const struct target *t )
{
    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 1.0f, 1.0f } } };   /* blue */
    VkRenderPassBeginInfo info = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = t->pass,
        .framebuffer = t->fb, .renderArea = { { 0, 0 }, { SIZE, SIZE } }, .clearValueCount = 1, .pClearValues = &clear };
    p_vkCmdBeginRenderPass( cb, &info, VK_SUBPASS_CONTENTS_INLINE );
}

/* after the pass: the image (in TRANSFER_SRC_OPTIMAL) into the pixel buffer, visible to the host */
static void copy_out( VkCommandBuffer cb, const struct target *t, const struct buffer *pixels )
{
    VkBufferImageCopy region = { .bufferRowLength = SIZE, .bufferImageHeight = SIZE,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { SIZE, SIZE, 1 } };
    VkMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT };

    p_vkCmdCopyImageToBuffer( cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, pixels->buffer, 1, &region );
    p_vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL );
}

static int pixel_is( const struct buffer *pixels, int x, int y, const uint8_t want[4], const char *what )
{
    const uint8_t *p = (const uint8_t *)pixels->map + ((size_t)y * SIZE + x) * 4;
    int i;
    for (i = 0; i < 4; i++)
    {
        if (abs( (int)p[i] - (int)want[i] ) > 2)
        {
            printf( "FAIL: geometry-shader draw: pixel (%d, %d) is %u %u %u %u, %s would be %u %u %u %u\n", x, y,
                    p[0], p[1], p[2], p[3], what, want[0], want[1], want[2], want[3] );
            return 0;
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- the tests */

static int geometry_draw( void )
{
    static const uint8_t green[4] = { 0, 255, 0, 255 }, blue[4] = { 0, 0, 255, 255 };
    static const int inside[][2] = { { 16, 16 }, { 9, 9 }, { 22, 22 }, { 9, 22 }, { 22, 9 } };
    static const int outside[][2] = { { 2, 2 }, { 29, 29 }, { 6, 16 }, { 16, 26 } };
    VkPipelineLayoutCreateInfo linfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    struct buffer pixels = { 0 };
    struct target t = { 0 };
    VkCommandBuffer cb;
    int ok = 0;
    size_t i;

    if (!make_target( &t ) || !make_buffer( SIZE * SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &pixels ) ||
        p_vkCreatePipelineLayout( device, &linfo, NULL, &layout ) != VK_SUCCESS)
        printf( "FAIL: geometry-shader draw: could not create the image, buffer or layout\n" );
    else if (!(pipeline = make_pipeline( &t, layout, gs_quad_vert, sizeof(gs_quad_vert), gs_quad_geom, sizeof(gs_quad_geom) )))
        printf( "FAIL: geometry-shader draw: no pipeline\n" );
    else if ((cb = begin()))
    {
        memset( pixels.map, 0, SIZE * SIZE * 4 );
        begin_pass( cb, &t );
        p_vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
        p_vkCmdDraw( cb, 3, 1, 0, 0 );
        p_vkCmdEndRenderPass( cb );
        copy_out( cb, &t, &pixels );
        if (submit( cb ))
        {
            ok = 1;
            for (i = 0; i < sizeof(inside) / sizeof(inside[0]); i++)
                ok &= pixel_is( &pixels, inside[i][0], inside[i][1], green, "the quad" );
            for (i = 0; i < sizeof(outside) / sizeof(outside[0]); i++)
                ok &= pixel_is( &pixels, outside[i][0], outside[i][1], blue, "the clear colour" );
            if (ok) printf( "PASS: geometry-shader draw (one triangle in, a 16 x 16 quad out)\n" );
        }
        else printf( "FAIL: geometry-shader draw: the submit failed\n" );
    }
    if (pipeline) p_vkDestroyPipeline( device, pipeline, NULL );
    if (layout) p_vkDestroyPipelineLayout( device, layout, NULL );
    free_buffer( &pixels );
    free_target( &t );
    return ok;
}

static int xfb_capture( void )
{
    VkPipelineLayoutCreateInfo linfo = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    struct buffer captured = { 0 }, counter = { 0 };
    VkDeviceSize offset = 0, size = 256, counter_offset = 0;
    VkMemoryBarrier host = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | VK_ACCESS_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    struct target t = { 0 };
    VkCommandBuffer cb;
    const float *f;
    uint32_t bytes;
    int ok = 0, i, k;

    if (!make_target( &t ) ||
        !make_buffer( size, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT, &captured ) ||
        !make_buffer( 16, VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT, &counter ) ||
        p_vkCreatePipelineLayout( device, &linfo, NULL, &layout ) != VK_SUCCESS)
        printf( "FAIL: transform-feedback capture: could not create the image, buffers or layout\n" );
    else if (!(pipeline = make_pipeline( &t, layout, xfb_vert, sizeof(xfb_vert), xfb_geom, sizeof(xfb_geom) )))
        printf( "FAIL: transform-feedback capture: no pipeline\n" );
    else if ((cb = begin()))
    {
        f = captured.map;
        for (i = 0; i < (int)(size / sizeof(float)); i++) ((float *)captured.map)[i] = -1.0f;
        memset( counter.map, 0, 16 );
        begin_pass( cb, &t );
        p_vkCmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline );
        p_vkCmdBindTransformFeedbackBuffersEXT( cb, 0, 1, &captured.buffer, &offset, &size );
        p_vkCmdBeginTransformFeedbackEXT( cb, 0, 0, NULL, NULL );
        p_vkCmdDraw( cb, 3, 1, 0, 0 );
        p_vkCmdEndTransformFeedbackEXT( cb, 0, 1, &counter.buffer, &counter_offset );
        p_vkCmdEndRenderPass( cb );
        p_vkCmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host,
                                0, NULL, 0, NULL );
        if (submit( cb ))
        {
            ok = 1;
            for (i = 0; i < 3; i++)
            {
                const float want[4] = { 100.0f + (float)i, 2.0f * (float)i, 3.0f * (float)i, 1.0f };
                for (k = 0; k < 4; k++) if (f[i * 4 + k] != want[k]) ok = 0;
                if (!ok)
                {
                    printf( "FAIL: transform-feedback capture: vertex %d is %g %g %g %g, wanted %g %g %g %g\n", i,
                            (double)f[i * 4], (double)f[i * 4 + 1], (double)f[i * 4 + 2], (double)f[i * 4 + 3],
                            (double)want[0], (double)want[1], (double)want[2], (double)want[3] );
                    break;
                }
            }
            if (ok && f[12] != -1.0f)
            {
                printf( "FAIL: transform-feedback capture: something was written past the three vertices (%g)\n", (double)f[12] );
                ok = 0;
            }
            memcpy( &bytes, counter.map, sizeof(bytes) );
            if (ok && bytes != 48)
            {
                printf( "FAIL: transform-feedback capture: the counter buffer says %u bytes, wanted 48\n", bytes );
                ok = 0;
            }
            if (ok) printf( "PASS: transform-feedback capture (3 vertices from the geometry shader, 48 bytes counted)\n" );
        }
        else printf( "FAIL: transform-feedback capture: the submit failed\n" );
    }
    if (pipeline) p_vkDestroyPipeline( device, pipeline, NULL );
    if (layout) p_vkDestroyPipelineLayout( device, layout, NULL );
    free_buffer( &counter );
    free_buffer( &captured );
    free_target( &t );
    return ok;
}

unsigned int vkgx_run( const struct vkgx_env *env, const struct vkgx_caps *caps )
{
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)env->gipa( env->instance, "vkGetDeviceProcAddr" );
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem = (PFN_vkGetPhysicalDeviceMemoryProperties)env->gipa(
        env->instance, "vkGetPhysicalDeviceMemoryProperties" );
    VkCommandPoolCreateInfo pinfo = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = env->family };
    unsigned int fails = 0;
    int loaded = 1;

    if (!caps->geometry)
    {
        printf( "SKIP: geometry-shader draw (no geometryShader)\n" );
        printf( "SKIP: transform-feedback capture (no geometryShader)\n" );
        return 0;
    }
    device = env->device;
    queue = env->queue;
    if (!gdpa || !get_mem) loaded = 0;
    else
    {
        get_mem( env->physical, &memprops );
#define X(name) if (!(p_##name = (PFN_##name)gdpa( device, #name ))) loaded = 0;
        DEVICE_FUNCS(X)
        if (caps->xfb) { XFB_FUNCS(X) }
#undef X
    }
    if (!loaded || p_vkCreateCommandPool( device, &pinfo, NULL, &pool ) != VK_SUCCESS)
    {
        printf( "FAIL: geometry/xfb: device functions or the command pool missing\n" );
        return VKGX_FAIL_GEOMETRY_DRAW | (caps->xfb ? VKGX_FAIL_XFB : 0);
    }
    if (!geometry_draw()) fails |= VKGX_FAIL_GEOMETRY_DRAW;
    if (!caps->xfb) printf( "SKIP: transform-feedback capture (no VK_EXT_transform_feedback with transformFeedback)\n" );
    else if (!xfb_capture()) fails |= VKGX_FAIL_XFB;
    p_vkDestroyCommandPool( device, pool, NULL );
    pool = VK_NULL_HANDLE;
    return fails;
}
