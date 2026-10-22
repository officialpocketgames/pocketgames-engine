// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part A4: the NSI providers' globals get a private prefix, so nothing they define can
// collide with ntdll's, win32u's or the server's unix code in the one app binary. These are the
// only non-static names of nsi/ndis.c, upstream ip.c, nsi/nsi_ip6scope.c, nsi/nsi_tcp.c and
// nsi/nsi_unix.c besides the two unix-call tables (Part A3), which keep their names.
// Included by ios_cfg_nsi.h, so every NSI file sees the same names.

#ifndef MYIOSDECK_IOS_NSI_RENAME_H
#define MYIOSDECK_IOS_NSI_RENAME_H

#define nsi_enumerate_all_ex        ios_nsi_enumerate_all_ex
#define nsi_get_all_parameters_ex   ios_nsi_get_all_parameters_ex
#define nsi_get_parameter_ex        ios_nsi_get_parameter_ex
#define convert_luid_to_unix_name   ios_nsi_convert_luid_to_unix_name
#define convert_unix_name_to_luid   ios_nsi_convert_unix_name_to_luid
#define get_ipv6_addr_scope_table   ios_nsi_get_ipv6_addr_scope_table
#define find_ipv6_addr_scope        ios_nsi_find_ipv6_addr_scope
#define ndis_module                 ios_nsi_ndis_module
#define ipv4_module                 ios_nsi_ipv4_module
#define ipv6_module                 ios_nsi_ipv6_module
#define tcp_module                  ios_nsi_tcp_module
#define udp_module                  ios_nsi_udp_module

/* netiodef.h defines these objects in the one file that sets __WINE_INIT_NPI_MODULEID
 * (nsi/nsi_unix.c) and declares them extern elsewhere */
#define NPI_MS_NDIS_MODULEID        ios_nsi_NPI_MS_NDIS_MODULEID
#define NPI_MS_IPV4_MODULEID        ios_nsi_NPI_MS_IPV4_MODULEID
#define NPI_MS_IPV6_MODULEID        ios_nsi_NPI_MS_IPV6_MODULEID
#define NPI_MS_TCP_MODULEID         ios_nsi_NPI_MS_TCP_MODULEID
#define NPI_MS_UDP_MODULEID         ios_nsi_NPI_MS_UDP_MODULEID

#endif /* MYIOSDECK_IOS_NSI_RENAME_H */
