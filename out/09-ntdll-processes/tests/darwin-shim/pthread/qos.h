/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Host syntax check only (tests/host-syntax-check.sh): the call of Apple's public
 * <pthread/qos.h> that ios_spawn.c uses. Never part of the iOS build.
 */
#ifndef MYIOSDECK_HOST_SHIM_PTHREAD_QOS_H
#define MYIOSDECK_HOST_SHIM_PTHREAD_QOS_H

#include <pthread.h>
#include <sys/qos.h>

int pthread_attr_set_qos_class_np( pthread_attr_t *attr, qos_class_t qos_class, int relative_priority );

#endif
