// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part C: 32-bit (WoW64) callers on iOS. A 32-bit program's address a
// is host address B + a, where B = ios_wow_base() is the base of its guest
// window (Wine's iOS unix side provides the function). Wine converts the outer
// args pointer of a unix call; every pointer inside the argument block is
// still a guest address.
//
// Everything here is plain C (it also compiles as C++) and takes the base as
// an argument, so it can be unit-tested on any host with a fake base.
// Include it after winemetal.h (the command record types).

#ifndef DXMT_IOS_WOW64_H
#define DXMT_IOS_WOW64_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* B of the calling thread's program; 0 if it has none (a 64-bit program).
   Defined by Wine's iOS unix side, which links into the same binary. */
unsigned long ios_wow_base(void);

#ifdef __cplusplus
}
#endif

/* The window is 4 GB; a guest address is always below that. */
#define DXMT_WOW_GUEST_MAX 0xFFFFFFFFull

/* Rule C4: the most records one encode call may carry. */
#define DXMT_WOW_MAX_CMD_RECORDS (1u << 20)

/* Pointer fields are 8 bytes in every argument struct (WMTMemoryPointer,
   WMTConstMemoryPointer, raw uint64_t); read and write them as integers. */
static inline uint64_t
dxmt_wow_load(const void *field) {
  uint64_t value;
  memcpy(&value, field, sizeof(value));
  return value;
}

static inline void
dxmt_wow_store(void *field, uint64_t value) {
  memcpy(field, &value, sizeof(value));
}

/* Rule C1, into the host: 0 stays NULL, anything else becomes base + value.
   A value with its high half set can't be a guest address: the field is left
   alone and false is returned (the caller fails the call). */
static inline bool
dxmt_wow_ptr_to_host(void *field, uint64_t base) {
  uint64_t guest = dxmt_wow_load(field);
  if (guest == 0)
    return true;
  if (guest > DXMT_WOW_GUEST_MAX)
    return false;
  dxmt_wow_store(field, base + guest);
  return true;
}

/* Rule C2, back to the program: NULL stays 0; a host address inside the
   window [base, base + 4 GB) becomes host - base; anything else becomes 0 and
   *inside is set to false (the caller logs it). The result fits in 32 bits, so
   the high half of the field is 0. */
static inline uint64_t
dxmt_wow_ptr_to_guest(uint64_t host, uint64_t base, bool *inside) {
  *inside = true;
  if (host == 0)
    return 0;
  if (host >= base && host - base <= DXMT_WOW_GUEST_MAX)
    return host - base;
  *inside = false;
  return 0;
}

/* ---- Rule C4: command chains of the encoders ------------------------------- */

/* Converts the pointer payload of one record (not its `next` link) into the
   host (to_host) or back. Must check every field before changing any, and
   return false (changing nothing) if one can't be converted. NULL: no payload. */
typedef bool (*dxmt_wow_payload_fn)(struct wmtcmd_base *cmd, uint64_t base, bool to_host);

/* One payload field: a guest pointer into the host, or a host pointer this
   code converted back to the guest. */
static inline bool
dxmt_wow_payload_field_ok(const void *field) {
  return dxmt_wow_load(field) <= DXMT_WOW_GUEST_MAX;
}

static inline void
dxmt_wow_payload_field_convert(void *field, uint64_t base, bool to_host) {
  uint64_t value = dxmt_wow_load(field);
  if (value)
    dxmt_wow_store(field, to_host ? value + base : value - base);
}

static inline void dxmt_wow_chain_to_guest(void *head_field, uint64_t base, dxmt_wow_payload_fn payload, long count);

/* Converts the chain whose head pointer is the 8-byte field `head_field` to
   host addresses in place: the head, every `next` link and every payload
   pointer. Returns the number of records (0 for an empty chain), or -1 when
   the chain can't be converted: a link with its high half set (which is what
   a record already converted looks like, so a cycle ends here), more than
   max_records records, or a payload the callback refuses. On -1 everything
   converted so far has been converted back.

   Needs base > DXMT_WOW_GUEST_MAX, so converted links can't pass for guest
   addresses. On iOS the low 4 GB are reserved, so every window lies above. */
static inline long
dxmt_wow_chain_to_host(void *head_field, uint64_t base, dxmt_wow_payload_fn payload, uint32_t max_records) {
  uint64_t guest = dxmt_wow_load(head_field);
  if (guest == 0)
    return 0;
  if (guest > DXMT_WOW_GUEST_MAX)
    return -1;
  dxmt_wow_store(head_field, base + guest);

  long count = 0;
  struct wmtcmd_base *cmd = (struct wmtcmd_base *)(uintptr_t)(base + guest);
  for (;;) {
    uint64_t next = dxmt_wow_load(&cmd->next);
    if ((uint64_t)count >= max_records || next > DXMT_WOW_GUEST_MAX)
      goto fail;
    if (payload && !payload(cmd, base, true))
      goto fail;
    count++;
    if (next == 0)
      return count;
    dxmt_wow_store(&cmd->next, base + next);
    cmd = (struct wmtcmd_base *)(uintptr_t)(base + next);
  }

fail:
  dxmt_wow_chain_to_guest(head_field, base, payload, count);
  return -1;
}

/* Converts the first `count` records of a chain that dxmt_wow_chain_to_host
   converted (and the head) back to guest addresses. */
static inline void
dxmt_wow_chain_to_guest(void *head_field, uint64_t base, dxmt_wow_payload_fn payload, long count) {
  uint64_t host = dxmt_wow_load(head_field);
  if (host == 0)
    return;
  dxmt_wow_store(head_field, host - base);

  struct wmtcmd_base *cmd = (struct wmtcmd_base *)(uintptr_t)host;
  for (long i = 0; i < count && cmd; i++) {
    uint64_t next = dxmt_wow_load(&cmd->next);
    if (payload)
      payload(cmd, base, false);
    dxmt_wow_store(&cmd->next, next ? next - base : 0);
    cmd = next ? (struct wmtcmd_base *)(uintptr_t)next : NULL;
  }
}

/* Payload pointers per command type (winemetal.h). Blit commands carry none. */
static inline bool
dxmt_wow_render_payload(struct wmtcmd_base *cmd, uint64_t base, bool to_host) {
  void *field = NULL;
  switch (cmd->type) {
  case WMTRenderCommandSetFragmentBytes:
    field = &((struct wmtcmd_render_setbytes *)cmd)->bytes;
    break;
  case WMTRenderCommandSetViewports:
    field = &((struct wmtcmd_render_setviewports *)cmd)->viewports;
    break;
  case WMTRenderCommandSetScissorRects:
    field = &((struct wmtcmd_render_setscissorrects *)cmd)->scissor_rects;
    break;
  default:
    return true;
  }
  if (to_host && !dxmt_wow_payload_field_ok(field))
    return false;
  dxmt_wow_payload_field_convert(field, base, to_host);
  return true;
}

static inline bool
dxmt_wow_compute_payload(struct wmtcmd_base *cmd, uint64_t base, bool to_host) {
  if (cmd->type != WMTComputeCommandSetBytes)
    return true;
  void *field = &((struct wmtcmd_compute_setbytes *)cmd)->bytes;
  if (to_host && !dxmt_wow_payload_field_ok(field))
    return false;
  dxmt_wow_payload_field_convert(field, base, to_host);
  return true;
}

#endif
