// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part C: handlers of the 32-bit unix-call table on iOS
// (dxmt_winemetal_unix_call_wow64_funcs). Included by winemetal_unix.c after
// the 64-bit handlers, which these wrap; the table itself is in that file.
//
// Rules (spec 06 C1-C7):
// - A pointer field the handler reads is a guest address: 0 stays NULL,
//   anything else becomes ios_wow_base() + value (nested and chained fields
//   too). Wrappers convert in a local copy of the argument struct (and of
//   const nested structs), so the program's memory is never left holding host
//   addresses.
// - A pointer field written for the program becomes host - ios_wow_base() if
//   it lies in the window, else 0 (logged).
// - Object handles pass through unchanged.
// - Encoder command chains are converted in place and converted back.
// - Entries without embedded pointers use the 64-bit handler directly (see the
//   table).

#define STATUS_INVALID_PARAMETER 0xC000000D
#define STATUS_NO_MEMORY 0xC0000017

static inline uint64_t
wow_base(void) {
  return (uint64_t)ios_wow_base();
}

static NTSTATUS
wow_bad_pointer(const char *what) {
  static _Atomic uint64_t count = 0;
  dxmt_ios_log_rare(&count, "32-bit %s: a pointer argument isn't a 32-bit address; call refused", what);
  return STATUS_INVALID_PARAMETER;
}

static void
wow_outside_window(const char *what) {
  static _Atomic uint64_t count = 0;
  dxmt_ios_log_rare(&count, "32-bit %s: a returned pointer lies outside the program's window; returned NULL", what);
}

/* An entry whose argument struct has exactly one pointer field (read by the
   handler) and whose other outputs are inline: convert the field in a local
   copy, run the 64-bit handler on the copy, copy the struct back with the
   program's original pointer value. */
#define WOW_ONE_POINTER(name, handler, type, field)                                                                    \
  static NTSTATUS name(void *obj) {                                                                                    \
    type *guest = obj;                                                                                                 \
    type p = *guest;                                                                                                   \
    uint64_t saved = dxmt_wow_load(&guest->field);                                                                     \
    if (!dxmt_wow_ptr_to_host(&p.field, wow_base()))                                                                   \
      return wow_bad_pointer(#handler);                                                                                \
    NTSTATUS status = handler(&p);                                                                                     \
    *guest = p;                                                                                                        \
    dxmt_wow_store(&guest->field, saved);                                                                              \
    return status;                                                                                                     \
  }

/* Rule C1 for entries with one pointer field. */
WOW_ONE_POINTER(wow_NSString_getCString, _NSString_getCString, struct unixcall_nsstring_getcstring, buffer_ptr)
WOW_ONE_POINTER(wow_MTLDevice_newSamplerState, _MTLDevice_newSamplerState, struct unixcall_mtldevice_newsamplerstate, info)
WOW_ONE_POINTER(
    wow_MTLDevice_newDepthStencilState, _MTLDevice_newDepthStencilState, struct unixcall_mtldevice_newdepthstencilstate,
    info
)
WOW_ONE_POINTER(wow_MTLLibrary_newFunction, _MTLLibrary_newFunction, struct unixcall_generic_obj_uint64_obj_ret, arg)
WOW_ONE_POINTER(
    wow_MTLCommandBuffer_renderCommandEncoder, _MTLCommandBuffer_renderCommandEncoder,
    struct unixcall_generic_obj_uint64_obj_ret, arg
)
WOW_ONE_POINTER(wow_MTLTexture_replaceRegion, _MTLTexture_replaceRegion, struct unixcall_mtltexture_replaceregion, data)
WOW_ONE_POINTER(
    wow_MTLDevice_newTemporalScaler, _MTLDevice_newTemporalScaler, struct unixcall_mtldevice_newfxtemporalscaler, info
)
WOW_ONE_POINTER(
    wow_MTLDevice_newSpatialScaler, _MTLDevice_newSpatialScaler, struct unixcall_mtldevice_newfxspatialscaler, info
)
WOW_ONE_POINTER(
    wow_MTLCommandBuffer_encodeTemporalScale, _MTLCommandBuffer_encodeTemporalScale,
    struct unixcall_mtlcommandbuffer_temporal_scale, props
)
WOW_ONE_POINTER(wow_NSString_string, _NSString_string, struct unixcall_nsstring_string, buffer_ptr)
WOW_ONE_POINTER(wow_NSString_alloc_init, _NSString_alloc_init, struct unixcall_nsstring_string, buffer_ptr)
WOW_ONE_POINTER(wow_MetalLayer_setProps, _MetalLayer_setProps, struct unixcall_generic_obj_constptr_noret, arg)
WOW_ONE_POINTER(wow_MetalLayer_getProps, _MetalLayer_getProps, struct unixcall_generic_obj_ptr_noret, arg)
WOW_ONE_POINTER(wow_MTLLogContainer_enumerate, _MTLLogContainer_enumerate, struct unixcall_enumerate, buffer)
WOW_ONE_POINTER(wow_WMTGetDisplayDescription, _WMTGetDisplayDescription, struct unixcall_generic_obj_ptr_noret, arg)
WOW_ONE_POINTER(wow_MetalLayer_getEDRValue, _MetalLayer_getEDRValue, struct unixcall_generic_obj_ptr_noret, arg)
WOW_ONE_POINTER(
    wow_WMTQueryDisplaySetting, _WMTQueryDisplaySetting, struct unixcall_query_display_setting, hdr_metadata
)
WOW_ONE_POINTER(
    wow_WMTUpdateDisplaySetting, _WMTUpdateDisplaySetting, struct unixcall_update_display_setting, hdr_metadata
)
WOW_ONE_POINTER(
    wow_WMTQueryDisplaySettingForLayer, _WMTQueryDisplaySettingForLayer, struct unixcall_query_display_setting_for_layer,
    hdr_metadata
)
WOW_ONE_POINTER(wow_MTLBuffer_updateContents, _MTLBuffer_updateContents, struct unixcall_mtlbuffer_updatecontents, data)
WOW_ONE_POINTER(
    wow_MTLDevice_newBinaryArchive, _MTLDevice_newBinaryArchive, struct unixcall_mtldevice_newbinaryarchive, url
)
WOW_ONE_POINTER(
    wow_MTLBinaryArchive_serialize, _MTLBinaryArchive_serialize, struct unixcall_mtlbinaryarchive_serialize, url
)
WOW_ONE_POINTER(wow_CacheReader_alloc_init, _CacheReader_alloc_init, struct unixcall_cache_alloc_init, path)
WOW_ONE_POINTER(wow_CacheReader_get, _CacheReader_get, struct unixcall_cache_get, key)
WOW_ONE_POINTER(wow_CacheWriter_alloc_init, _CacheWriter_alloc_init, struct unixcall_cache_alloc_init, path)
WOW_ONE_POINTER(wow_CacheWriter_set, _CacheWriter_set, struct unixcall_cache_set, key)
WOW_ONE_POINTER(wow_WMTSetMetalShaderCachePath, _WMTSetMetalShaderCachePath, struct unixcall_setmetalcachepath, path)

/* ---- retain / release: Part E counts ---------------------------------------- */

static NTSTATUS
wow_NSObject_retain(void *obj) {
  dxmt_ios_wow_retain(dxmt_wow_load(obj));
  return STATUS_SUCCESS;
}

static NTSTATUS
wow_NSObject_release(void *obj) {
  dxmt_ios_wow_release(dxmt_wow_load(obj));
  return STATUS_SUCCESS;
}

/* ---- buffers and textures: B4 and Part E records ----------------------------- */

static NTSTATUS
wow_MTLDevice_newBuffer(void *obj) {
  static atomic_bool refused_logged = false;
  struct unixcall_mtldevice_newbuffer *guest = obj;
  struct unixcall_mtldevice_newbuffer p = *guest;
  uint64_t base = wow_base();
  if (!dxmt_wow_ptr_to_host(&p.info, base) || !p.info.ptr)
    return wow_bad_pointer("MTLDevice_newBuffer");
  struct WMTBufferInfo *info = p.info.ptr;

  /* B4: Metal-allocated CPU-visible memory has no 32-bit address. Storage
     mode is bits 4-5 of the options: 0 Shared, 1 Managed (CPU-visible),
     2 Private, 3 Memoryless. Never hand out a truncated host pointer. */
  uint64_t memory = dxmt_wow_load(&info->memory);
  uint64_t storage = ((uint64_t)info->options >> 4) & 3;
  if (memory == 0 && storage < 2) {
    if (!atomic_exchange(&refused_logged, true))
      fprintf(stderr, "dxmt: 32-bit newBuffer of CPU-visible storage without its own memory refused\n");
    guest->ret = 0;
    return STATUS_SUCCESS;
  }
  if (!dxmt_wow_ptr_to_host(&info->memory, base))
    return wow_bad_pointer("MTLDevice_newBuffer memory");

  NTSTATUS status = _MTLDevice_newBuffer(&p);

  /* C2: `memory` is in/out; give the program back a guest address */
  bool inside;
  dxmt_wow_store(&info->memory, dxmt_wow_ptr_to_guest(dxmt_wow_load(&info->memory), base, &inside));
  if (!inside)
    wow_outside_window("MTLDevice_newBuffer");
  guest->ret = p.ret;
  dxmt_ios_wow_created(p.ret);
  return status;
}

static NTSTATUS
wow_MTLDevice_newTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *guest = obj;
  struct unixcall_mtldevice_newtexture p = *guest;
  if (!dxmt_wow_ptr_to_host(&p.info, wow_base()) || !p.info.ptr)
    return wow_bad_pointer("MTLDevice_newTexture");
  NTSTATUS status = _MTLDevice_newTexture(&p);
  guest->ret = p.ret;
  dxmt_ios_wow_created(p.ret);
  return status;
}

static NTSTATUS
wow_MTLDevice_newSharedTexture(void *obj) {
  struct unixcall_mtldevice_newtexture *guest = obj;
  struct unixcall_mtldevice_newtexture p = *guest;
  if (!dxmt_wow_ptr_to_host(&p.info, wow_base()) || !p.info.ptr)
    return wow_bad_pointer("MTLDevice_newSharedTexture");
  NTSTATUS status = _MTLDevice_newSharedTexture(&p);
  guest->ret = p.ret;
  dxmt_ios_wow_created(p.ret);
  return status;
}

static NTSTATUS
wow_MTLBuffer_newTexture(void *obj) {
  struct unixcall_mtlbuffer_newtexture *guest = obj;
  struct unixcall_mtlbuffer_newtexture p = *guest;
  if (!dxmt_wow_ptr_to_host(&p.info, wow_base()) || !p.info.ptr)
    return wow_bad_pointer("MTLBuffer_newTexture");
  NTSTATUS status = _MTLBuffer_newTexture(&p);
  guest->ret = p.ret;
  dxmt_ios_wow_created(p.ret);
  return status;
}

/* No pointers; recorded for Part E (a view keeps its texture alive). */
static NTSTATUS
wow_MTLTexture_newTextureView(void *obj) {
  struct unixcall_mtltexture_newtextureview *params = obj;
  NTSTATUS status = _MTLTexture_newTextureView(params);
  dxmt_ios_wow_created(params->ret);
  return status;
}

/* ---- pipelines: nested binary-archive arrays ----------------------------------- */

static NTSTATUS
wow_MTLDevice_newComputePipelineState(void *obj) {
  struct unixcall_mtldevice_newcomputepso *guest = obj;
  struct unixcall_mtldevice_newcomputepso p = *guest;
  uint64_t base = wow_base();
  if (!dxmt_wow_ptr_to_host(&p.info, base) || !p.info.ptr)
    return wow_bad_pointer("MTLDevice_newComputePipelineState");
  struct WMTComputePipelineInfo info = *(const struct WMTComputePipelineInfo *)p.info.ptr;
  if (!dxmt_wow_ptr_to_host(&info.binary_archives_for_lookup, base))
    return wow_bad_pointer("MTLDevice_newComputePipelineState archives");
  p.info.ptr = &info;
  NTSTATUS status = _MTLDevice_newComputePipelineState(&p);
  guest->ret_error = p.ret_error;
  guest->ret_pso = p.ret_pso;
  return status;
}

static NTSTATUS
wow_MTLDevice_newRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newrenderpso *guest = obj;
  struct unixcall_mtldevice_newrenderpso p = *guest;
  uint64_t base = wow_base();
  if (!dxmt_wow_ptr_to_host(&p.info, base) || !p.info.ptr)
    return wow_bad_pointer("MTLDevice_newRenderPipelineState");
  struct WMTRenderPipelineInfo info = *(const struct WMTRenderPipelineInfo *)p.info.ptr;
  if (!dxmt_wow_ptr_to_host(&info.binary_archives_for_lookup, base))
    return wow_bad_pointer("MTLDevice_newRenderPipelineState archives");
  p.info.ptr = &info;
  NTSTATUS status = _MTLDevice_newRenderPipelineState(&p);
  guest->ret_error = p.ret_error;
  guest->ret_pso = p.ret_pso;
  return status;
}

static NTSTATUS
wow_MTLDevice_newMeshRenderPipelineState(void *obj) {
  struct unixcall_mtldevice_newmeshrenderpso *guest = obj;
  struct unixcall_mtldevice_newmeshrenderpso p = *guest;
  uint64_t base = wow_base();
  if (!dxmt_wow_ptr_to_host(&p.info, base) || !p.info.ptr)
    return wow_bad_pointer("MTLDevice_newMeshRenderPipelineState");
  struct WMTMeshRenderPipelineInfo info = *(const struct WMTMeshRenderPipelineInfo *)p.info.ptr;
  if (!dxmt_wow_ptr_to_host(&info.binary_archives_for_lookup, base))
    return wow_bad_pointer("MTLDevice_newMeshRenderPipelineState archives");
  p.info.ptr = &info;
  NTSTATUS status = _MTLDevice_newMeshRenderPipelineState(&p);
  guest->ret_error = p.ret_error;
  guest->ret_pso = p.ret_pso;
  return status;
}

/* ---- other nested pointers -------------------------------------------------------- */

static NTSTATUS
wow_MTLCaptureManager_startCapture(void *obj) {
  struct unixcall_mtlcapturemanager_startcapture *guest = obj;
  struct unixcall_mtlcapturemanager_startcapture p = *guest;
  uint64_t base = wow_base();
  if (!dxmt_wow_ptr_to_host(&p.info, base) || !p.info.ptr)
    return wow_bad_pointer("MTLCaptureManager_startCapture");
  struct WMTCaptureInfo info = *(const struct WMTCaptureInfo *)p.info.ptr;
  if (!dxmt_wow_ptr_to_host(&info.output_url, base))
    return wow_bad_pointer("MTLCaptureManager_startCapture url");
  p.info.ptr = &info;
  NTSTATUS status = _MTLCaptureManager_startCapture(&p);
  guest->ret = p.ret;
  return status;
}

/* At most this many function constants per call (a sanity bound). */
#define WOW_MAX_FUNCTION_CONSTANTS 4096

static NTSTATUS
wow_MTLLibrary_newFunctionWithConstants(void *obj) {
  struct unixcall_mtllibrary_newfunction_with_constants *guest = obj;
  struct unixcall_mtllibrary_newfunction_with_constants p = *guest;
  uint64_t base = wow_base();
  if (!dxmt_wow_ptr_to_host(&p.name, base) || !dxmt_wow_ptr_to_host(&p.constants, base))
    return wow_bad_pointer("MTLLibrary_newFunctionWithConstants");
  if (p.num_constants > WOW_MAX_FUNCTION_CONSTANTS || (p.num_constants && !p.constants.ptr))
    return wow_bad_pointer("MTLLibrary_newFunctionWithConstants constants");
  struct WMTFunctionConstant *constants = NULL;
  if (p.num_constants) {
    constants = malloc(sizeof(*constants) * p.num_constants);
    if (!constants)
      return STATUS_NO_MEMORY;
    memcpy(constants, p.constants.ptr, sizeof(*constants) * p.num_constants);
    for (uint64_t i = 0; i < p.num_constants; i++) {
      if (!dxmt_wow_ptr_to_host(&constants[i].data, base)) {
        free(constants);
        return wow_bad_pointer("MTLLibrary_newFunctionWithConstants data");
      }
    }
    p.constants.ptr = constants;
  }
  NTSTATUS status = _MTLLibrary_newFunctionWithConstants(&p);
  free(constants);
  guest->ret = p.ret;
  guest->ret_error = p.ret_error;
  return status;
}

/* `handle` is the data pointer. Below 4 GB it is a guest address (bytecode in
   the program's memory). Above, it is a host address this unix half handed
   out itself: the Data field of SM50GetCompiledBitcode, which the Windows half
   passes back here without reading it (rule C3, an opaque value). */
static NTSTATUS
wow_DispatchData_alloc_init(void *obj) {
  struct unixcall_generic_obj_uint64_obj_ret *guest = obj;
  struct unixcall_generic_obj_uint64_obj_ret p = *guest;
  if (p.handle && p.handle <= DXMT_WOW_GUEST_MAX)
    p.handle += wow_base();
  NTSTATUS status = _DispatchData_alloc_init(&p);
  guest->ret = p.ret;
  return status;
}

/* ---- C4: encoder command chains ---------------------------------------------------- */

static NTSTATUS
wow_encode_commands(void *obj, NTSTATUS (*handler)(void *), dxmt_wow_payload_fn payload, const char *what) {
  static _Atomic uint64_t refused = 0;
  struct unixcall_generic_obj_cmd_noret *guest = obj;
  struct unixcall_generic_obj_cmd_noret p = *guest;
  uint64_t base = wow_base();
  long count = dxmt_wow_chain_to_host(&p.cmd_head, base, payload, DXMT_WOW_MAX_CMD_RECORDS);
  if (count < 0) {
    dxmt_ios_log_rare(&refused, "32-bit %s: command chain has a cycle, a bad link or more than %u records; refused",
                      what, DXMT_WOW_MAX_CMD_RECORDS);
    return STATUS_INVALID_PARAMETER;
  }
  NTSTATUS status = handler(&p);
  /* the Windows half reuses the records */
  dxmt_wow_chain_to_guest(&p.cmd_head, base, payload, count);
  return status;
}

static NTSTATUS
wow_MTLBlitCommandEncoder_encodeCommands(void *obj) {
  return wow_encode_commands(obj, _MTLBlitCommandEncoder_encodeCommands, NULL, "blit encodeCommands");
}

static NTSTATUS
wow_MTLComputeCommandEncoder_encodeCommands(void *obj) {
  return wow_encode_commands(
      obj, _MTLComputeCommandEncoder_encodeCommands, dxmt_wow_compute_payload, "compute encodeCommands"
  );
}

static NTSTATUS
wow_MTLRenderCommandEncoder_encodeCommands(void *obj) {
  return wow_encode_commands(
      obj, _MTLRenderCommandEncoder_encodeCommands, dxmt_wow_render_payload, "render encodeCommands"
  );
}

/* ---- C7: the control call answers op 7 only --------------------------------------- */

static NTSTATUS
wow_DXMTIOS_control(void *obj) {
  struct unixcall_dxmt_ios_control *params = obj;
  if (params->op != DXMT_IOS_CONTROL_AVAILABLE_MEMORY)
    return STATUS_NOT_IMPLEMENTED;
  return _DXMTIOS_control(obj);
}

static NTSTATUS
unix_call_not_implemented(void *args) {
  return STATUS_NOT_IMPLEMENTED;
}
