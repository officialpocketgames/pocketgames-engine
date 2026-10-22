// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part F2: the one fixed hardware identity games see (registry and SMBIOS). The values
// are in ios_hwident.c, in one table.

#ifndef MYIOSDECK_IOS_HWIDENT_H
#define MYIOSDECK_IOS_HWIDENT_H

struct ios_hw_identity
{
    /* processor (registry HARDWARE\DESCRIPTION\System\CentralProcessor\N and SMBIOS type 4) */
    const char    *cpu_vendor;        /* VendorIdentifier */
    const char    *cpu_name;          /* ProcessorNameString */
    const char    *cpu_identifier;    /* Identifier */
    unsigned char  cpu_smbios_family; /* SMBIOS processor family */
    /* firmware (SMBIOS type 0, registry ...\System\BIOS) */
    const char    *bios_vendor;
    const char    *bios_version;
    const char    *bios_date;         /* mm/dd/yyyy */
    const char    *bios_system_version; /* registry ...\System SystemBiosVersion */
    /* system (SMBIOS type 1) */
    const char    *sys_vendor;
    const char    *sys_product;
    const char    *sys_version;
    const char    *sys_serial;
    const char    *sys_sku;
    const char    *sys_family;
    unsigned char  sys_uuid[16];
    /* baseboard (SMBIOS type 2) and chassis (type 3) */
    const char    *board_vendor;
    const char    *board_product;
    const char    *board_version;
    const char    *board_serial;
    const char    *chassis_vendor;
    unsigned char  chassis_type;      /* 3: desktop */
    const char    *chassis_serial;
};

extern const struct ios_hw_identity ios_hw_identity;

/* the processor clock to report, in MHz (from CNTFRQ_EL0, see ios_plausible_mhz) */
extern unsigned int ios_hw_cpu_mhz(void);

#endif /* MYIOSDECK_IOS_HWIDENT_H */
