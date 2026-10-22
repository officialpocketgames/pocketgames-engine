/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 Part A10: which services start automatically. Pure code shared by services.c and the
 * host tests. Start types by number (winsvc.h): SERVICE_BOOT_START 0, SERVICE_SYSTEM_START 1,
 * SERVICE_AUTO_START 2, SERVICE_DEMAND_START 3, SERVICE_DISABLED 4.
 */

#ifndef MYIOSDECK_SERVICES_AUTOSTART_H
#define MYIOSDECK_SERVICES_AUTOSTART_H

#define AUTOSTART_BOOT_START    0u
#define AUTOSTART_SYSTEM_START  1u
#define AUTOSTART_AUTO_START    2u
#define AUTOSTART_DEMAND_START  3u
#define AUTOSTART_DISABLED      4u

/* Boot, system and auto start, and root PnP device services whatever their start type, as
 * upstream, except that a disabled service never starts automatically (also a PnP one). */
static inline int services_autostart( unsigned int start_type, int root_pnp )
{
    if (start_type == AUTOSTART_DISABLED) return 0;
    return start_type == AUTOSTART_BOOT_START || start_type == AUTOSTART_SYSTEM_START ||
           start_type == AUTOSTART_AUTO_START || root_pnp;
}

/* for the log line */
static inline const char *services_start_type_name( unsigned int start_type )
{
    switch (start_type)
    {
    case AUTOSTART_BOOT_START:   return "boot";
    case AUTOSTART_SYSTEM_START: return "system";
    case AUTOSTART_AUTO_START:   return "auto";
    case AUTOSTART_DEMAND_START: return "demand";
    case AUTOSTART_DISABLED:     return "disabled";
    default:                     return "unknown";
    }
}

#endif /* MYIOSDECK_SERVICES_AUTOSTART_H */
