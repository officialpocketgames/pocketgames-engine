/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Host syntax check only (tests/host-syntax-check.sh): the quality-of-service class names of
 * Apple's public <sys/qos.h>. Never part of the iOS build.
 */
#ifndef MYIOSDECK_HOST_SHIM_SYS_QOS_H
#define MYIOSDECK_HOST_SHIM_SYS_QOS_H

typedef enum
{
    QOS_CLASS_USER_INTERACTIVE = 0x21,
    QOS_CLASS_USER_INITIATED   = 0x19,
    QOS_CLASS_DEFAULT          = 0x15,
    QOS_CLASS_UTILITY          = 0x11,
    QOS_CLASS_BACKGROUND       = 0x09,
    QOS_CLASS_UNSPECIFIED      = 0x00,
} qos_class_t;

#endif
