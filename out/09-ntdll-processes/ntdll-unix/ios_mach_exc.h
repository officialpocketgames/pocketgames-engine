// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part C: the Mach exception server (ios_mach_exc.c).

#ifndef MYIOSDECK_IOS_MACH_EXC_H
#define MYIOSDECK_IOS_MACH_EXC_H

/* Creates the port and its server thread and installs the port as the task's EXC_BAD_ACCESS
 * port. Once, before the first Windows thread runs (signal_init_process). */
extern void ios_mach_exc_init(void);

#endif /* MYIOSDECK_IOS_MACH_EXC_H */
