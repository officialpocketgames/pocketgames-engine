/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Host syntax check only (tests/host-syntax-check.sh): the few Mach declarations ntdll's iOS
 * code uses, written from Apple's public documentation of the calls, so a Linux compiler can
 * check those files the way the iOS build does (undeclared functions are errors). Never part of
 * the iOS build, which uses the SDK's headers.
 */
#ifndef MYIOSDECK_HOST_SHIM_MACH_H
#define MYIOSDECK_HOST_SHIM_MACH_H

#include <stdint.h>

typedef int          kern_return_t;
typedef unsigned int mach_port_t;
typedef mach_port_t  vm_map_t, task_t, thread_t, mem_entry_name_port_t, host_name_port_t;
typedef uintptr_t    vm_address_t, vm_offset_t, vm_size_t;
typedef uint64_t     mach_vm_address_t, mach_vm_size_t;
typedef int          vm_prot_t, vm_inherit_t, boolean_t;
typedef unsigned int mach_msg_type_number_t, natural_t;
typedef int          vm_region_flavor_t, task_flavor_t;
typedef int         *vm_region_info_t, *task_info_t;

#define KERN_SUCCESS          0
#define KERN_INVALID_ADDRESS  1
#define KERN_NO_SPACE         3
#define MACH_PORT_NULL        0
#define MEMORY_OBJECT_NULL    0
#define VM_FLAGS_FIXED        0
#define VM_PROT_NONE          0
#define VM_PROT_READ          1
#define VM_PROT_WRITE         2
#define VM_PROT_EXECUTE       4
#define VM_PROT_ALL           7
#define VM_INHERIT_COPY       1
#define MACH_VM_MAX_ADDRESS   0x0000000fc0000000ull
#define VM_REGION_BASIC_INFO_64 9
#define TASK_VM_INFO          22

typedef struct
{
    vm_prot_t protection, max_protection;
    vm_inherit_t inheritance;
    boolean_t shared, reserved;
    uint64_t offset;
    int behavior;
    unsigned short user_wired_count;
} vm_region_basic_info_data_64_t;
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / sizeof(int)))

typedef struct
{
    uint64_t virtual_size;
    int      region_count, page_size;
    uint64_t resident_size, resident_size_peak, device, device_peak, internal, internal_peak,
             external, external_peak, reusable, reusable_peak, purgeable_volatile_pmap,
             purgeable_volatile_resident, purgeable_volatile_virtual, compressed, compressed_peak,
             compressed_lifetime, phys_footprint;
    uint64_t min_address, max_address;
} task_vm_info_data_t;
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / sizeof(natural_t)))

mach_port_t mach_task_self(void);
mach_port_t mach_thread_self(void);
kern_return_t task_info( task_t task, task_flavor_t flavor, task_info_t info, mach_msg_type_number_t *count );
kern_return_t vm_map( vm_map_t target, vm_address_t *address, vm_size_t size, vm_address_t mask, int flags,
                      mem_entry_name_port_t object, vm_offset_t offset, boolean_t copy, vm_prot_t cur,
                      vm_prot_t max, vm_inherit_t inheritance );
kern_return_t vm_deallocate( vm_map_t target, vm_address_t address, vm_size_t size );
kern_return_t vm_region_64( vm_map_t target, vm_address_t *address, vm_size_t *size, vm_region_flavor_t flavor,
                            vm_region_info_t info, mach_msg_type_number_t *count, mach_port_t *object );
kern_return_t vm_read_overwrite( vm_map_t target, vm_address_t address, vm_size_t size, vm_address_t data,
                                 vm_size_t *out_size );
kern_return_t mach_port_deallocate( mach_port_t task, mach_port_t name );

#endif
