// Changed for MYIOSDECK (spec 05 B7, C, D5; spec 06 A). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
// Changed for MYIOSDECK (spec 05 item 90: the release marked in the memory ledger).
// Changed for MYIOSDECK (spec 05 item 91: on iOS shared textures go through the app's share table,
// never fail only because sharing isn't available, get a minimal keyed mutex, and are logged).
// Changed for MYIOSDECK (spec 05 item 94: the automatic mip clamp by DXMT's texture total, any format).
#include "util_bitness.hpp"
#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "d3d11_enumerable.hpp"
#include <atomic>
#ifdef DXMT_IOS
#include "config/config.hpp"
#include "dxmt_mem_ledger.hpp"  // MYIOSDECK spec 05 item 94
#include "util_env.hpp"         // MYIOSDECK spec 05 item 94
#include "d3d11_share.hpp"
#include "d3d11_trace_state.hpp"
#include "dxgi_keyed_mutex.hpp"
#include "dxmt_format.hpp"
#include "dxmt_mip_clamp.hpp"
#include <memory>
#include <mutex>
#endif
#include "d3d11_view.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_staging.hpp"
#include "dxmt_texture.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"

namespace dxmt {

#ifdef DXMT_IOS
/* MYIOSDECK spec 05 item 91: each D3DKMT call of the sharing paths, with its result */
static void
NoteKmt(const char *call, NTSTATUS status, const std::string &detail) {
  share::Note(
      std::string(call) + (status ? " failed" : ""), status != 0, share::KmtLine(call, uint32_t(status), detail)
  );
}
#define DXMT_NOTE_KMT(call, status, detail) NoteKmt(call, status, detail)
#else
#define DXMT_NOTE_KMT(call, status, detail) ((void)0)
#endif

#pragma region DeviceTexture

template <typename tag_texture>
class DeviceTexture : public TResourceBase<tag_texture, IMTLMinLODClampable> {
private:
  Rc<Texture> underlying_texture_;
  Rc<RenamableTexturePool> renamable_;
  float min_lod = 0.0;
  D3DKMT_HANDLE local_kmt_ = 0;
  D3DKMT_HANDLE global_kmt_ = 0;

  using SRVBase =
      TResourceViewBase<tag_shader_resource_view<DeviceTexture<tag_texture>>>;
  class TextureSRV : public SRVBase {
  private:
    TextureViewKey view_key_;

  public:
    TextureSRV(TextureViewKey view_key,
               const tag_shader_resource_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : SRVBase(pDesc, pResource, pDevice), view_key_(view_key) {}

    Rc<Buffer> buffer() final { return {}; };
    Rc<Texture> texture() final { return this->resource->underlying_texture_; };
    unsigned viewId() final { return view_key_;};
    BufferSlice bufferSlice() final { return {};}
  };

  using UAVBase =
      TResourceViewBase<tag_unordered_access_view<DeviceTexture<tag_texture>>>;
  class TextureUAV : public UAVBase {
  private:
    TextureViewKey view_key_;

  public:
    TextureUAV(TextureViewKey view_key,
               const tag_unordered_access_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : UAVBase(pDesc, pResource, pDevice), view_key_(view_key){}

    Rc<Buffer> buffer() final { return {}; };
    Rc<Texture> texture() final { return this->resource->underlying_texture_; };
    unsigned viewId() final { return view_key_;};
    BufferSlice bufferSlice() final { return {};}
    Rc<Buffer> counter() final { return {}; };
  };

  using RTVBase =
      TResourceViewBase<tag_render_target_view<DeviceTexture<tag_texture>>>;
  class TextureRTV : public RTVBase {
  private:
    TextureViewKey view_key_;
    WMTPixelFormat view_format_;
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;

  public:
    TextureRTV(
        TextureViewKey view_key, WMTPixelFormat view_format, const tag_render_target_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &mtl_rtv_desc
    ) :
        RTVBase(pDesc, pResource, pDevice),
        view_key_(view_key),
        view_format_(view_format),
        attachment_desc(mtl_rtv_desc) {}

    WMTPixelFormat
    pixelFormat() final {
      return view_format_;
    }

    MTL_RENDER_PASS_ATTACHMENT_DESC &description() final {
      return attachment_desc;
    };

    Rc<Texture> texture() final {
      return this->resource->underlying_texture_;
    }

    unsigned viewId() final {
      return view_key_;
    }
  };

  using DSVBase =
      TResourceViewBase<tag_depth_stencil_view<DeviceTexture<tag_texture>>>;
  class TextureDSV : public DSVBase {
  private:
    TextureViewKey view_key_;
    WMTPixelFormat view_format_;
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;

  public:
    TextureDSV(
        TextureViewKey view_key, WMTPixelFormat view_format, const tag_depth_stencil_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &attachment_desc
    ) :
        DSVBase(pDesc, pResource, pDevice),
        view_key_(view_key),
        view_format_(view_format),
        attachment_desc(attachment_desc) {}

    WMTPixelFormat
    pixelFormat() final {
      return view_format_;
    }

    MTL_RENDER_PASS_ATTACHMENT_DESC &description() final {
      return attachment_desc;
    };

    UINT readonlyFlags() final {
      return this->desc.Flags;
    };

    Rc<Texture> texture() final {
      return this->resource->underlying_texture_;
    }

    unsigned viewId() final {
      return view_key_;
    }

    dxmt::Rc<dxmt::RenamableTexturePool> renamable() final {
      return this->resource->renamable_;
    }
  };

#ifdef DXMT_IOS
  /* MYIOSDECK spec 05 item 91: the IDXGIKeyedMutex of a D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
     texture, made or opened, shared or not (dxgi_keyed_mutex.hpp) */
  std::unique_ptr<MTLDXGIKeyedMutex<DeviceTexture<tag_texture>>> keyed_mutex_;

  void
  initKeyedMutex() {
    if (this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)
      keyed_mutex_ = std::make_unique<MTLDXGIKeyedMutex<DeviceTexture<tag_texture>>>(this);
  }
#else
  void
  initKeyedMutex() {}
#endif

public:
  DeviceTexture(const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, MTLD3D11Device *pDevice) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      underlying_texture_(std::move(u_texture)) {
    initKeyedMutex();
  }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<RenamableTexturePool> &&renamable,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      underlying_texture_(std::move(u_texture)),
      renamable_(std::move(renamable)) {
    initKeyedMutex();
  }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, D3DKMT_HANDLE localHandle, D3DKMT_HANDLE globalHandle,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      underlying_texture_(std::move(u_texture)), local_kmt_(localHandle), global_kmt_(globalHandle) {
    initKeyedMutex();
  }

#ifdef DXMT_IOS
  /* MYIOSDECK spec 05 item 91 */
  HRESULT
  QueryExtraInterface(REFIID riid, void **ppvObject) override {
    if (keyed_mutex_ && riid == __uuidof(IDXGIKeyedMutex)) {
      *ppvObject = ref(static_cast<IDXGIKeyedMutex *>(keyed_mutex_.get()));
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  /* ReleaseSync: what was written before it is committed before the other side's work */
  void
  FlushForKeyedMutex() {
    this->m_parent->GetImmediateContextPrivate()->Flush();
  }
#endif

  ~DeviceTexture() {
    if (underlying_texture_.ptr()) // MYIOSDECK spec 05 item 90: what still lives waits for the GPU
      underlying_texture_->releasedByProgram();
    if (local_kmt_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = this->m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt_;
      D3DKMTDestroyAllocation(&destroy);
    }
  }

  Rc<Buffer> buffer() final { return {}; };
  Rc<Texture> texture() final { return this->underlying_texture_; };
  BufferSlice bufferSlice() final { return {};}
  Rc<StagingResource> staging(UINT) final { return nullptr; }
  Rc<DynamicBuffer> dynamicBuffer(UINT*, UINT*) final { return {}; }
  Rc<DynamicLinearTexture> dynamicLinearTexture(UINT*, UINT*) final { return {}; };
  Rc<DynamicBuffer> dynamicTexture(UINT , UINT *, UINT *) final { return {}; };

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
                                 ID3D11RenderTargetView1 **ppView) override {
    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->underlying_texture_.ptr(), finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureRTV(key, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
                                 ID3D11DepthStencilView **ppView) override {
    D3D11_DEPTH_STENCIL_VIEW_DESC finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->underlying_texture_.ptr(), finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureDSV(key, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      ERR("DeviceTexture: Failed to create SRV descriptor");
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->underlying_texture_.ptr(), finalDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture SRV");
      return E_FAIL;
    }
#ifdef DXMT_IOS
    // Spec 05 D5: the view's mip range is logical; the Metal view uses physical
    // levels (clamped into range). finalDesc, which GetDesc returns, stays logical.
    if (uint32_t bias = underlying_texture_->mipBias()) {
      uint32_t first, count;
      mipclamp::TranslateViewRange(
          bias, underlying_texture_->mipLevelCount(), descriptor.firstMiplevel, descriptor.miplevelCount, first, count
      );
      descriptor.firstMiplevel = first;
      descriptor.miplevelCount = count;
    }
#endif
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureSRV(key, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
                            ID3D11UnorderedAccessView1 **ppView) override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->underlying_texture_.ptr(), finalDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture UAV");
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    TextureViewKey key = underlying_texture_->createView(descriptor);
    *ppView = ref(new TextureUAV(key, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  virtual HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    if (pSharedHandle == nullptr || (this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
      return E_INVALIDARG;
    }

    if (!(this->desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
      *pSharedHandle = NULL;
      return S_OK;
    }

    if (!global_kmt_) {
      return E_INVALIDARG;
    }

    *pSharedHandle = reinterpret_cast<HANDLE>(global_kmt_);
    return S_OK;
  }

  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    InitReturnPtr(pNTHandle);
    if (!local_kmt_)
      return E_INVALIDARG;

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES*>(Attributes);

    WCHAR buffer[MAX_PATH];
    UNICODE_STRING name_str;
    if (pName) {
      DWORD session, len, name_len = wcslen(pName);

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
      memcpy(buffer + len, pName, (name_len + 1) * sizeof(WCHAR));
      name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer;

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    NTSTATUS status = D3DKMTShareObjects(1, &local_kmt_, &attr, Access, pNTHandle);
    DXMT_NOTE_KMT( // MYIOSDECK spec 05 item 91
        "D3DKMTShareObjects", status,
        "shared texture" + (pName ? ", name " + str::format(pName) : std::string()) +
            (status ? std::string() : ", NT handle " + str::format(*pNTHandle))
    );
    if (status) {
      ERR("DeviceTexture: Failed to create shared handle");
      return E_FAIL;
    }

    return S_OK;
  }

  void SetMinLOD(float MinLod) override { min_lod = MinLod; }

  float GetMinLOD() override { return min_lod; }
};

struct SharedResourceData {
  char mach_port_name[54];
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc;
};

#ifdef DXMT_IOS
/**
Spec 05 D5: the number of top mip levels to leave out of the Metal texture of
a 2D (2D array, cube) BC texture with more than one level.
- d3d11.mipClampBC = N (default 0) clamps every such texture by N;
- d3d11.mipClampAuto (default off) clamps textures of 1024 or more texels in
  width or height by 1 while the process has less than d3d11.mipClampAutoMB
  (default 1536) MB of headroom (os_proc_available_memory through the control
  call); an explicit mipClampBC > 0 wins.
The policy itself is in dxmt_mip_clamp.hpp.

MYIOSDECK spec 05 item 94: the automatic clamp is on in both bitnesses (d3d11.mipClampAuto=false
or DXMT_MIP_CLAMP_AUTO=0 turns it off), and besides the 32-bit headroom rule above (BC only) it
has the total rule, any format: a sampled-only 2D, 2D array or cube texture with 2 or more mips,
DEFAULT or IMMUTABLE, created while DXMT's live texture total is over 1.5 GB drops its top level,
over 3 GB its top two. Render targets, depth, UAVs, shared, staging and dynamic textures never
clamp (the last two are made elsewhere). An explicit mipClampBC still wins for BC textures.
*/
static uint32_t
MipClampBias(const WMTTextureInfo &info, UINT Usage, UINT BindFlags, UINT CPUAccessFlags, UINT MiscFlags) {
  if (info.mipmap_level_count <= 1)
    return 0;
  switch (info.type) {
  case WMTTextureType2D:
  case WMTTextureType2DArray:
  case WMTTextureTypeCube:
  case WMTTextureTypeCubeArray:
    break;
  default:
    return 0;
  }
  if (MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))
    return 0;
  const bool bc = IsBlockCompressionFormat(info.pixel_format);

  static const int32_t explicit_bias = Config::getInstance().getOption<int32_t>("d3d11.mipClampBC", 0);
  // MYIOSDECK spec 05 item 94: on in both bitnesses (spec 06 A had it on in the i386 build only)
  static const bool auto_enabled = mipclamp::AutoClampSwitch(
      Config::getInstance().getOption<bool>("d3d11.mipClampAuto", true), env::getEnvVar("DXMT_MIP_CLAMP_AUTO").c_str()
  );
  static const int32_t auto_mb = Config::getInstance().getOption<int32_t>("d3d11.mipClampAutoMB", 1536);
  static std::mutex state_lock;
  static mipclamp::AutoState state;

  uint32_t requested = 0;
  if (bc) {
    // D5: an explicit clamp, or (spec 06 A) the 32-bit headroom rule
    std::lock_guard<std::mutex> lock(state_lock);
    requested = mipclamp::RequestedBias(
        explicit_bias > 0 ? uint32_t(explicit_bias) : 0u, auto_enabled && kI386Build,
        uint64_t(auto_mb > 0 ? auto_mb : 0) << 20, info.width, info.height, state,
        [](uint64_t &headroom) { return WMTQueryAvailableMemory(&headroom); }
    );
    if (explicit_bias > 0) // an explicit mipClampBC wins
      return mipclamp::ChooseBias(info.width, info.height, info.mipmap_level_count, requested);
  }
  // MYIOSDECK spec 05 item 94: DXMT's own texture total
  mipclamp::TotalRuleTexture candidate;
  candidate.type_ok = true;
  candidate.mips = info.mipmap_level_count;
  candidate.usage = Usage;
  candidate.bind = BindFlags;
  candidate.cpu = CPUAccessFlags;
  candidate.shared = false;
  if (auto_enabled && mipclamp::TotalRuleCandidate(candidate)) {
    const uint64_t total = memledger::Global().textureLiveBytes();
    const uint32_t total_bias = mipclamp::TotalBias(total);
    if (total_bias > requested) {
      requested = total_bias;
      // the first clamp at each level, once per process
      static std::atomic<uint32_t> logged_level{0};
      uint32_t previous = logged_level.load(std::memory_order_relaxed);
      while (total_bias > previous && !logged_level.compare_exchange_weak(previous, total_bias)) {
      }
      if (total_bias > previous)
        Logger::info(mipclamp::TotalClampLine(total, total_bias));
    }
  }
  return bc ? mipclamp::ChooseBias(info.width, info.height, info.mipmap_level_count, requested)
            : mipclamp::ChooseBiasAny(info.mipmap_level_count, requested);
}

/* MYIOSDECK spec 05 item 91 */
template <typename DESC>
uint32_t
DescHeight(const DESC &desc) {
  if constexpr (requires { desc.Height; })
    return desc.Height;
  else
    return 1;
}

/* "handle 0x40000042", or what an NT-handle texture has instead (no global handle) */
inline std::string
ShareHandleText(D3DKMT_HANDLE handle) {
  return handle ? "handle " + share::Hex(handle)
                : std::string("no global handle (an NT handle comes from CreateSharedHandle)");
}

/* MYIOSDECK spec 05 item 91: a texture that asks for sharing, on iOS. Every Windows program runs
   in the app's one process on one MTLDevice, so the shared texture is the MTLTexture itself, in
   the app's share table (MTLDevice_newSharedTexture registers it; the allocation's machPort is
   its key). The key travels in D3DKMT's runtime data as a share name, where the mach port's
   bootstrap name used to; an opener finds it there (ImportSharedTexture*).
   Sharing can be unavailable: the table is full, there is no D3DKMT device, or
   D3DKMTCreateAllocation2 fails. Then the texture is made as not shareable, with the line saying
   why, and the create still succeeds. Nothing is kept on any path: the key goes back to the
   table and no D3DKMT allocation is left. */
template <typename tag, typename Initialize>
HRESULT
CreateSharedDeviceTextureIOS(
    MTLD3D11Device *pDevice, const typename tag::DESC1 *pDesc, const typename tag::DESC1 &finalDesc,
    Rc<Texture> &&texture, Initialize &&initialize, typename tag::COM_IMPL **ppTexture
) {
  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::GpuPrivate);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  flags.set(TextureAllocationFlag::Shared);
  auto allocation = texture->allocate(flags);
  const uint32_t key = allocation->machPort; // the share table's key; 0: not registered

  auto line = [&](const std::string &outcome) {
    return share::TextureLine(
        finalDesc.Width, DescHeight(finalDesc), TraceDxgiFormat(finalDesc.Format), finalDesc.MiscFlags,
        finalDesc.BindFlags, outcome
    );
  };

  std::string why;
  D3DKMT_CREATEALLOCATION create = {};
  if (!allocation->texture())
    why = "no Metal texture";
  else if (!key)
    why = "the share table is full";
  else if (!(pDevice->GetLocalD3DKMT() & 0xc0000000))
    why = "no D3DKMT device";
  else {
    SharedResourceData runtimeData = {};
    share::ShareName(key, runtimeData.mach_port_name);
    runtimeData.dimension = tag::dimension;
    memcpy(&runtimeData.desc, pDesc, sizeof(typename tag::DESC1));

    create.hDevice = pDevice->GetLocalD3DKMT();
    create.pPrivateRuntimeData = &runtimeData;
    create.PrivateRuntimeDataSize = sizeof(runtimeData);
    create.Flags.StandardAllocation = 1;
    create.NumAllocations = 1;
    D3DDDI_ALLOCATIONINFO2 allocationInfo = {};
    create.pAllocationInfo2 = &allocationInfo;
    D3DKMT_CREATESTANDARDALLOCATION standardAllocation = {};
    create.pStandardAllocation = &standardAllocation;
    standardAllocation.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    create.Flags.ExistingSysMem = 1;
    D3DDDI_ALLOCATIONINFO systemMem = {};
    allocationInfo.pSystemMem = &systemMem;
    create.Flags.CreateResource = 1;
    create.Flags.CreateShared = 1;
    create.Flags.NtSecuritySharing = !!(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    NTSTATUS status = D3DKMTCreateAllocation2(&create);
    share::Note(
        status ? "D3DKMTCreateAllocation2 failed" : "D3DKMTCreateAllocation2", status != 0,
        share::KmtLine(
            "D3DKMTCreateAllocation2", uint32_t(status),
            "share key " + std::to_string(key) + (status ? std::string() : ", " + ShareHandleText(create.hGlobalShare))
        )
    );
    if (status)
      why = "D3DKMTCreateAllocation2 failed";
  }

  if (!why.empty()) {
    if (key) {
      WMTShareRelease(key);
      allocation->machPort = 0;
    }
    share::Note("shared texture not shareable", true, line("not shareable (" + why + ")"));
    initialize(std::move(allocation));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice))
    );
    return S_OK;
  }

  share::Note("shared texture", false, line(ShareHandleText(create.hGlobalShare) + ", share key " + std::to_string(key)));
  initialize(std::move(allocation));
  *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
      ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), create.hResource, create.hGlobalShare, pDevice))
  );
  return S_OK;
}
#endif

template <typename tag>
HRESULT CreateDeviceTextureInternal(MTLD3D11Device *pDevice,
                                    const typename tag::DESC1 *pDesc,
                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                    typename tag::COM_IMPL **ppTexture) {
  WMTTextureInfo info;
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }
  // Spec 05 D5: `info` becomes the physical description; finalDesc (what the
  // game sees) stays logical.
  uint32_t mip_bias = 0;
#ifdef DXMT_IOS
  mip_bias = MipClampBias(
      info, finalDesc.Usage, finalDesc.BindFlags, finalDesc.CPUAccessFlags, finalDesc.MiscFlags
  ); // MYIOSDECK spec 05 item 94: the description's usage and binding too
  if (mip_bias) {
    info.width = std::max(1u, info.width >> mip_bias);
    info.height = std::max(1u, info.height >> mip_bias);
    info.mipmap_level_count = info.mipmap_level_count - mip_bias;
  }
#endif
  bool single_subresource = info.mipmap_level_count == 1 && info.array_length == 1 &&
                            !(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE);
  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
#ifdef DXMT_IOS
  texture->setMipBias(mip_bias);
#endif

  auto &initializer = pDevice->GetDXMTDevice().queue().initializer;

  // Subresources are enumerated in logical levels; levels above the bias have
  // no physical counterpart and are skipped, the rest move up by the bias.
  auto initialize = [&](Rc<TextureAllocation> &&allocation) {
    texture->rename(std::move(allocation));
    if (!pInitialData) {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        if (sub.MipLevel < mip_bias)
          continue;
        initializer.initWithZero(texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel - mip_bias);
      }
    } else {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        if (sub.MipLevel < mip_bias)
          continue;
        auto &data = pInitialData[sub.SubresourceId];
        initializer.initWithData(
            texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel - mip_bias, data.pSysMem,
            data.SysMemPitch, data.SysMemSlicePitch
        );
      }
    }
  };

  auto shared_flag =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (finalDesc.MiscFlags & shared_flag) {
#ifdef DXMT_IOS
    // MYIOSDECK spec 05 item 91 (replaces B7's mach-port path): the app's share table
    return CreateSharedDeviceTextureIOS<tag>(pDevice, pDesc, finalDesc, std::move(texture), initialize, ppTexture);
#else
    if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
      ERR("DeviceTexture: Invalid device handle");
      return E_FAIL;
    }
    // use a dedicated path for now, because there are other works for private storage mode

    Flags<TextureAllocationFlag> flags;
    flags.set(TextureAllocationFlag::GpuPrivate);
    if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(TextureAllocationFlag::GpuReadonly);
    flags.set(TextureAllocationFlag::Shared);
    auto allocation = texture->allocate(flags);

    mach_port_t mach_port = allocation->machPort;
    if (!mach_port) {
      ERR("DeviceTexture: Failed to get mach port for shared texture");
      return E_FAIL;
    }
    SharedResourceData runtimeData;
    MakeUniqueSharedName(runtimeData.mach_port_name);
    if (!WMTBootstrapRegister(runtimeData.mach_port_name, mach_port)) {
      ERR("DeviceTexture: Failed to register mach port for shared texture");
      return E_FAIL;
    }
    runtimeData.dimension = tag::dimension;
    memcpy(&runtimeData.desc, pDesc, sizeof(typename tag::DESC1));

    D3DKMT_CREATEALLOCATION create = {};
    create.hDevice = pDevice->GetLocalD3DKMT();
    create.pPrivateRuntimeData = &runtimeData;
    create.PrivateRuntimeDataSize = sizeof(runtimeData);
    create.Flags.StandardAllocation = 1;
    create.NumAllocations = 1;
    D3DDDI_ALLOCATIONINFO2 allocationInfo = {};
    create.pAllocationInfo2 = &allocationInfo;
    D3DKMT_CREATESTANDARDALLOCATION standardAllocation = {};
    create.pStandardAllocation = &standardAllocation;
    standardAllocation.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    create.Flags.ExistingSysMem = 1;
    D3DDDI_ALLOCATIONINFO systemMem;
    allocationInfo.pSystemMem = &systemMem;
    create.Flags.CreateResource = 1;
    create.Flags.CreateShared = 1;
    create.Flags.NtSecuritySharing = !!(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE);
    if (D3DKMTCreateAllocation2(&create)) {
      ERR("DeviceTexture: Failed to create D3DKMT for shared texture");
      return E_FAIL;
    }

    // TODO: handle keyed mutex

    initialize(std::move(allocation));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), create.hResource,
                                   create.hGlobalShare, pDevice))
    );
    return S_OK;
#endif
  }

  Flags<TextureAllocationFlag> flags;
  flags.set(finalDesc.CPUAccessFlags ? TextureAllocationFlag::GpuManaged : TextureAllocationFlag::GpuPrivate);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  if (single_subresource && (finalDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    Rc<RenamableTexturePool> renamable = new RenamableTexturePool(texture.ptr(), 32, flags);
    initialize(renamable->getNext(0));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), std::move(renamable), pDevice)));
  } else {
    initialize(texture->allocate(flags));
#ifdef DXMT_IOS
    // MYIOSDECK spec 05 item 94: the clamp counted, with about what it saved, for [dxmt-mem]
    if (mip_bias && texture->current())
      memledger::Global().noteMipClamp(mipclamp::ClampSavedBytes(texture->current()->allocatedBytes, mip_bias));
#endif
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice)));
  }
  return S_OK;
}

HRESULT
CreateDeviceTexture1D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE1D_DESC *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture1D **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_1d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture2D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE2D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture2D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture3D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE3D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture3D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_3d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

/* `handle` names what the program opened, for the lines (MYIOSDECK spec 05 item 91) */
template <typename tag>
HRESULT
ImportSharedTextureInternal(
    MTLD3D11Device *pDevice, const typename tag::DESC1 *pDescUnchecked, mach_port_t MachPort, REFIID riid,
    void **ppTexture, const std::string &handle
) {
  WMTTextureInfo info;
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDescUnchecked, &finalDesc, &info))) {
#ifdef DXMT_IOS
    share::Note(
        "open shared mismatch", true,
        share::OpenLine("texture", handle, "mismatch", "its description isn't valid on this device")
    );
#endif
    return E_INVALIDARG;
  }

  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
#ifdef DXMT_IOS
  /* MYIOSDECK spec 05 item 91: MachPort is the share key. The opened texture wraps the same
     MTLTexture; what the opener asks for (the creator's description, from the runtime data) must
     be what that texture is, or the open is refused. A refused open's allocation goes at once,
     and with it its use of the table entry. */
  WMTTextureInfo found = {};
  auto allocation = texture->import(MachPort, &found);
  const std::string key_text = "share key " + std::to_string(MachPort);
  if (!allocation) {
    share::Note("open shared missing", true, share::OpenLine("texture", handle, "missing", key_text));
    return E_INVALIDARG;
  }
  const std::string diff = share::DescriptionDiff(share::DescOf(info), share::DescOf(found));
  if (!diff.empty()) {
    share::Note("open shared mismatch", true, share::OpenLine("texture", handle, "mismatch", diff));
    return E_INVALIDARG;
  }
  share::Note("open shared found", false, share::OpenLine("texture", handle, "found", key_text));
#else
  auto allocation = texture->import(MachPort);
  if (!allocation)
    return E_FAIL;
#endif
  texture->rename(std::move(allocation));

  Com<DeviceTexture<tag>> device_texture = (ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice)));
  return device_texture->QueryInterface(riid, ppTexture);
}

/* The mach port (macOS: through the bootstrap server) or the share key (iOS, MYIOSDECK spec 05
   item 91: the name in the runtime data) that the creator left for an opener. */
static bool
LookUpShared(const SharedResourceData &runtimeData, const std::string &handle, mach_port_t *port) {
#ifdef DXMT_IOS
  uint32_t key = 0;
  if (!share::ParseShareName(runtimeData.mach_port_name, sizeof(runtimeData.mach_port_name), &key)) {
    share::Note(
        "open shared missing", true, share::OpenLine("texture", handle, "missing", "no share key in its runtime data")
    );
    return false;
  }
  *port = key;
  return true;
#else
  if (!WMTBootstrapLookUp(runtimeData.mach_port_name, port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return false;
  }
  return true;
#endif
}

static HRESULT
ImportSharedByDimension(
    MTLD3D11Device *pDevice, SharedResourceData &runtimeData, mach_port_t port, REFIID riid, void **ppTexture,
    const std::string &handle
) {
  switch (runtimeData.dimension) {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(
        pDevice, &runtimeData.desc.desc1d, port, riid, ppTexture, handle
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(
        pDevice, &runtimeData.desc.desc2d, port, riid, ppTexture, handle
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(
        pDevice, &runtimeData.desc.desc3d, port, riid, ppTexture, handle
    );
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTexture(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (!(reinterpret_cast<uintptr_t>(hResource) & 0xc0000000)) {
    WARN("ImportSharedTexture: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  const std::string handle = "handle " + str::format(hResource); // MYIOSDECK spec 05 item 91
  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFO query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  NTSTATUS status = D3DKMTQueryResourceInfo(&query);
  DXMT_NOTE_KMT("D3DKMTQueryResourceInfo", status, handle);
  if (status) {
    WARN("ImportSharedTexture: Failed to query resource: ", hResource);
    return E_INVALIDARG;
  }

  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN("ImportSharedTexture: Unexpected size: ", query.PrivateRuntimeDataSize);
    return E_INVALIDARG;
  }

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCE open = {};
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

  status = D3DKMTOpenResource2(&open);
  DXMT_NOTE_KMT("D3DKMTOpenResource2", status, handle);
  if (status) {
    WARN("ImportSharedTexture: Failed to open resource: ", hResource);
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  mach_port_t mach_port;
  if (!LookUpShared(runtimeData, handle, &mach_port))
    return E_INVALIDARG;

  return ImportSharedByDimension(pDevice, runtimeData, mach_port, riid, ppTexture, handle);
}

HRESULT
ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("ImportSharedTextureFromNtHandle: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  const std::string handle = "NT handle " + str::format(hResource); // MYIOSDECK spec 05 item 91
  struct SharedResourceData runtimeData;

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  NTSTATUS status = D3DKMTQueryResourceInfoFromNtHandle(&query);
  DXMT_NOTE_KMT("D3DKMTQueryResourceInfoFromNtHandle", status, handle);
  if (status) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }
  
  if (query.PrivateRuntimeDataSize != sizeof(runtimeData)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
  char dummy;

  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
  open.pTotalPrivateDriverDataBuffer = &dummy;
  open.TotalPrivateDriverDataBufferSize = 0;

  status = D3DKMTOpenResourceFromNtHandle(&open);
  DXMT_NOTE_KMT("D3DKMTOpenResourceFromNtHandle", status, handle);
  if (status) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  if (open.hSyncObject) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled sync object"));
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroySync = {};
    destroySync.hSyncObject = open.hSyncObject;
    D3DKMTDestroySynchronizationObject(&destroySync);
  }
  if (open.hKeyedMutex) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Ignoring bundled keyed mutex"));
    D3DKMT_DESTROYKEYEDMUTEX destroyMutex = {};
    destroyMutex.hKeyedMutex = open.hKeyedMutex;
    D3DKMTDestroyKeyedMutex(&destroyMutex);
  }

  mach_port_t mach_port;
  if (!LookUpShared(runtimeData, handle, &mach_port))
    return E_INVALIDARG;

  return ImportSharedByDimension(pDevice, runtimeData, mach_port, riid, ppTexture, handle);
}

HRESULT
ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
) {
  D3DKMT_OPENNTHANDLEFROMNAME openFromName = {};
  openFromName.dwDesiredAccess = dwDesiredAccess;

  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);

  WCHAR buffer[MAX_PATH];
  UNICODE_STRING name_str;
  DWORD session, len, name_len = wcslen(lpName);

  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
  memcpy(buffer + len, lpName, (name_len + 1) * sizeof(WCHAR));
  name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
  name_str.MaximumLength += sizeof(WCHAR);
  name_str.Buffer = buffer;

  attr.ObjectName = &name_str;
  attr.Attributes = OBJ_CASE_INSENSITIVE;
  openFromName.pObjAttrib = &attr;

  NTSTATUS status = D3DKMTOpenNtHandleFromName(&openFromName);
  DXMT_NOTE_KMT("D3DKMTOpenNtHandleFromName", status, "name " + str::format(lpName)); // MYIOSDECK spec 05 item 91
  if (status) {
    WARN(str::format("ImportSharedTextureByName: Failed to open NT handle from name: ", lpName));
    return E_INVALIDARG;
  }

  HRESULT res = ImportSharedTextureFromNtHandle(pDevice, openFromName.hNtHandle, riid, ppTexture);
  CloseHandle(openFromName.hNtHandle);
  return res;
}

#pragma endregion

} // namespace dxmt