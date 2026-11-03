// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 34 Part E3: when the driver refuses a device or an instance, say what was missing. The
// line "[vk-ios] device refused: missing geometryShader, ..." is the first thing to read when a
// program fails. Features are compared field by field: VkPhysicalDeviceFeatures, and every
// feature struct of the request's chain whose layout ios_vk_features_gen.h knows (the Vulkan
// 1.1-1.4 structs among them), queried from the driver with the same chain.

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ios_vk_internal.h"

struct feature_struct
{
    VkStructureType stype;
    size_t          size;
    unsigned int    count;
    const char     *fields;   /* names, each ended by a NUL */
};

/* where the first VkBool32 of an sType'd feature struct lies (after sType and pNext) */
#define FIRST_BOOL offsetof(VkPhysicalDeviceVulkan11Features, storageBuffer16BitAccess)

/* the generator's field counts and first members, checked against the compiler's layout */
#define IVK_CORE_FEATURES(n, names) \
    _Static_assert( sizeof(VkPhysicalDeviceFeatures) == 4 * (n), "VkPhysicalDeviceFeatures" );
#define IVK_FEATURE_STRUCT(type, stype, n, first, names) \
    _Static_assert( offsetof(type, first) == FIRST_BOOL && \
                    sizeof(type) == ((FIRST_BOOL + 4 * (n) + 7) & ~(size_t)7), #type );
#include "ios_vk_features_gen.h"
#undef IVK_CORE_FEATURES
#undef IVK_FEATURE_STRUCT

#define IVK_CORE_FEATURES(n, names)
#define IVK_FEATURE_STRUCT(type, stype, n, first, names) { stype, sizeof(type), n, names },
static const struct feature_struct feature_structs[] =
{
#include "ios_vk_features_gen.h"
};
#undef IVK_CORE_FEATURES
#undef IVK_FEATURE_STRUCT

#define IVK_CORE_FEATURES(n, names) static const unsigned int core_count = (n); static const char core_fields[] = names;
#define IVK_FEATURE_STRUCT(type, stype, n, first, names)
#include "ios_vk_features_gen.h"
#undef IVK_CORE_FEATURES
#undef IVK_FEATURE_STRUCT

static const struct feature_struct *find_feature_struct( VkStructureType stype )
{
    size_t i;
    for (i = 0; i < sizeof(feature_structs) / sizeof(feature_structs[0]); i++)
        if (feature_structs[i].stype == stype) return &feature_structs[i];
    return NULL;
}

static pthread_mutex_t refusal_lock = PTHREAD_MUTEX_INITIALIZER;

static int first_refusal( struct ivk_pidset *set )
{
    int first;
    pthread_mutex_lock( &refusal_lock );
    first = ivk_pid_first( set, ivk_process_id() );
    pthread_mutex_unlock( &refusal_lock );
    return first;
}

static void missing_extensions( const char *const *names, uint32_t count, const VkExtensionProperties *props,
                                uint32_t nprops, struct ivk_names *out )
{
    uint32_t i, j;

    for (i = 0; i < count; i++)
    {
        for (j = 0; j < nprops; j++) if (!strcmp( names[i], props[j].extensionName )) break;
        if (j == nprops) ivk_names_add( out, names[i] );
    }
}

void ivk_explain_device_refusal( struct ivk_instance *instance, VkPhysicalDevice physical,
                                 const VkDeviceCreateInfo *info, VkResult res )
{
    static struct ivk_pidset logged;
    VkPhysicalDeviceFeatures2 have = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    const VkPhysicalDeviceFeatures *want_core = info->pEnabledFeatures;
    PFN_vkGetPhysicalDeviceFeatures2 get2;
    VkBaseOutStructure *copies[64], *tail = (VkBaseOutStructure *)&have;
    const VkBaseInStructure *wanted[64];
    const VkBaseInStructure *s;
    VkExtensionProperties *props = NULL;
    unsigned int ncopies = 0, unchecked = 0, i;
    uint32_t nprops = 0;
    struct ivk_names names;
    char unknown[96] = "";
    size_t unknown_len = 0;

    if (res != VK_ERROR_FEATURE_NOT_PRESENT && res != VK_ERROR_EXTENSION_NOT_PRESENT) return;
    if (!first_refusal( &logged )) return;
    ivk_names_init( &names );

    /* extensions the program asked for that the driver doesn't offer */
    if (instance->fn.EnumerateDeviceExtensionProperties &&
        instance->fn.EnumerateDeviceExtensionProperties( physical, NULL, &nprops, NULL ) == VK_SUCCESS &&
        nprops && (props = calloc( nprops, sizeof(*props) )) &&
        instance->fn.EnumerateDeviceExtensionProperties( physical, NULL, &nprops, props ) >= 0)
        missing_extensions( info->ppEnabledExtensionNames, info->enabledExtensionCount, props, nprops, &names );
    free( props );

    /* the driver's answer for the same feature structs */
    for (s = info->pNext; s; s = s->pNext)
    {
        const struct feature_struct *fs;
        VkBaseOutStructure *copy;

        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
        {
            want_core = &((const VkPhysicalDeviceFeatures2 *)s)->features;
            continue;
        }
        if (!(fs = find_feature_struct( s->sType )))
        {
            /* not a feature struct this table knows (or not a feature struct at all) */
            int n = snprintf( unknown + unknown_len, sizeof(unknown) - unknown_len, "%s%d",
                              unchecked ? " " : "", (int)s->sType );
            if (n > 0 && (size_t)n < sizeof(unknown) - unknown_len) unknown_len += (size_t)n;
            unchecked++;
            continue;
        }
        if (ncopies == sizeof(copies) / sizeof(copies[0]) || !(copy = calloc( 1, fs->size ))) continue;
        copy->sType = s->sType;
        tail->pNext = copy;
        tail = copy;
        wanted[ncopies] = s;
        copies[ncopies++] = copy;
    }
    get2 = instance->fn.GetPhysicalDeviceFeatures2KHR ? instance->fn.GetPhysicalDeviceFeatures2KHR
                                                       : instance->fn.GetPhysicalDeviceFeatures2;
    if (get2) get2( physical, &have );
    else if (instance->fn.GetPhysicalDeviceFeatures) instance->fn.GetPhysicalDeviceFeatures( physical, &have.features );

    if (want_core)
        ivk_bool_diff( (const uint32_t *)want_core, (const uint32_t *)&have.features, core_count, core_fields, &names );
    for (i = 0; i < ncopies; i++)
    {
        const struct feature_struct *fs = find_feature_struct( copies[i]->sType );
        if (get2)
            ivk_bool_diff( (const uint32_t *)((const char *)wanted[i] + FIRST_BOOL),
                           (const uint32_t *)((const char *)copies[i] + FIRST_BOOL), fs->count, fs->fields, &names );
        free( copies[i] );
    }

    if (names.count)
        ivk_log( "device refused: missing %s%s%s", names.buf, unchecked ? "; structs not checked: " : "", unknown );
    else
        ivk_log( "device refused (%d): nothing missing found%s%s", (int)res,
                 unchecked ? "; structs not checked: " : "", unknown );
}

void ivk_explain_instance_refusal( const VkInstanceCreateInfo *info, VkResult res )
{
    static struct ivk_pidset logged;
    PFN_vkEnumerateInstanceExtensionProperties enumerate;
    VkExtensionProperties *props = NULL;
    uint32_t nprops = 0;
    struct ivk_names names;

    if (res != VK_ERROR_EXTENSION_NOT_PRESENT && res != VK_ERROR_LAYER_NOT_PRESENT) return;
    if (!first_refusal( &logged )) return;
    ivk_names_init( &names );
    enumerate = (PFN_vkEnumerateInstanceExtensionProperties)IOS_VK_DRIVER_GIPA( NULL, "vkEnumerateInstanceExtensionProperties" );
    if (enumerate && enumerate( NULL, &nprops, NULL ) == VK_SUCCESS && nprops &&
        (props = calloc( nprops, sizeof(*props) )) && enumerate( NULL, &nprops, props ) >= 0)
        missing_extensions( info->ppEnabledExtensionNames, info->enabledExtensionCount, props, nprops, &names );
    free( props );
    if (names.count) ivk_log( "instance refused: missing %s", names.buf );
    else ivk_log( "instance refused (%d): nothing missing found", (int)res );
}
