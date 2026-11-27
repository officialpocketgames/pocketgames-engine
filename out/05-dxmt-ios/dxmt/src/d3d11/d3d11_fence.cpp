// Changed for MYIOSDECK (spec 05 clarification 63: QueryInterface counted). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
// Changed for MYIOSDECK (spec 05 item 91: on iOS a shared fence's event goes through the app's share
// table, a create never fails only because sharing isn't available, and the D3DKMT calls are logged).
#include "d3d11_fence.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"
#ifdef DXMT_IOS
#include "d3d11_share.hpp"
#endif

namespace dxmt {

#ifdef DXMT_IOS
/* MYIOSDECK spec 05 item 91: each D3DKMT call of the fence's sharing, with its result */
static void
NoteFenceKmt(const char *call, NTSTATUS status, const std::string &detail) {
  share::Note(
      std::string(call) + (status ? " failed" : ""), status != 0, share::KmtLine(call, uint32_t(status), detail)
  );
}
#endif

class MTLD3D11FenceImpl : public MTLD3D11DeviceChild<MTLD3D11Fence> {
public:
  MTLD3D11FenceImpl(
      MTLD3D11Device *pDevice, WMT::Reference<WMT::SharedEvent> event, D3DKMT_HANDLE handle, uint32_t share_key = 0
  ) :
      MTLD3D11DeviceChild<MTLD3D11Fence>(pDevice) {
    this->event = std::move(event);
    local_kmt = handle;
    share_key_ = share_key;
  };

  ~MTLD3D11FenceImpl() {
    if (local_kmt) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
      destroy.hSyncObject = local_kmt;
      D3DKMTDestroySynchronizationObject(&destroy);
    }
#ifdef DXMT_IOS
    if (share_key_) // MYIOSDECK spec 05 item 91: this fence was a user of the table entry
      WMTShareRelease(share_key_);
#endif
  };

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    D11_CHILD_CALL(QueryInterface); // MYIOSDECK spec 05 clarification 63
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Fence)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11Query), riid)) {
      WARN("D3D11Fence: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(const SECURITY_ATTRIBUTES *pAttributes, DWORD Access,
                     const WCHAR *Name, HANDLE *pHandle) final {
    InitReturnPtr(pHandle);
    if (!local_kmt)
      return E_INVALIDARG;

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES*>(pAttributes);

    WCHAR buffer[MAX_PATH];
    UNICODE_STRING name_str;
    if (Name) {
      DWORD session, len, name_len = wcslen(Name);

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
      memcpy(buffer + len, Name, (name_len + 1) * sizeof(WCHAR));
      name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer;

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    NTSTATUS status = D3DKMTShareObjects(1, &local_kmt, &attr, Access, pHandle);
#ifdef DXMT_IOS
    NoteFenceKmt( // MYIOSDECK spec 05 item 91
        "D3DKMTShareObjects", status,
        "shared fence" + (Name ? ", name " + str::format(Name) : std::string()) +
            (status ? std::string() : ", NT handle " + str::format(*pHandle))
    );
#endif
    if (status) {
      ERR("D3D11Fence: Failed to create shared handle");
      return E_FAIL;
    }

    return S_OK;
  };

  UINT64 STDMETHODCALLTYPE GetCompletedValue() final {
    return event.signaledValue();
  };

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 Value,
                                                 HANDLE Event) final {
    auto shared_event_listener = this->m_parent->GetDXMTDevice().queue().GetSharedEventListener();
    MTLSharedEvent_setWin32EventAtValue(event.handle, shared_event_listener, Event, Value);
    return S_OK;
  };

private:
  uint32_t share_key_ = 0; // MYIOSDECK spec 05 item 91: the share table's key (iOS; 0: not shared)
};

#ifdef DXMT_IOS
/* MYIOSDECK spec 05 item 91: a shared fence on iOS. Its MTLSharedEvent goes in the app's share
   table, and the key, as a share name, into D3DKMT through the escape the mach port's name used.
   Sharing can be unavailable (no D3DKMT device, no sync object, a full table): the fence is made
   as not shareable, with the line saying why, and the create succeeds; what was made for the
   sharing is given back.
   Wine 11.4's D3DKMTEscape updates a resource's runtime data, not a sync object's, so the name may
   not reach an opener (OpenSharedFence then logs it missing); its result is logged, and the fence
   stays shareable at the D3DKMT level (CreateSharedHandle works). */
static void
ShareFenceIOS(MTLD3D11Device *pDevice, WMT::SharedEvent event, D3DKMT_HANDLE &local_kmt, uint32_t &share_key) {
  std::string why;
  if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
    why = "no D3DKMT device";
  } else {
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {};
    create.hDevice = pDevice->GetLocalD3DKMT();
    create.Info.Type = D3DDDI_FENCE;
    create.Info.Flags.Shared = 1;
    create.Info.Flags.NtSecuritySharing = 1;
    NTSTATUS status = D3DKMTCreateSynchronizationObject2(&create);
    NoteFenceKmt(
        "D3DKMTCreateSynchronizationObject2", status,
        status ? std::string("shared fence") : "shared fence, sync object " + share::Hex(create.hSyncObject)
    );
    if (status) {
      why = "D3DKMTCreateSynchronizationObject2 failed";
    } else {
      local_kmt = create.hSyncObject;
      share_key = WMTShareEvent(event.handle);
      if (!share_key) {
        why = "the share table is full";
      } else {
        char name[share::kNameSize];
        share::ShareName(share_key, name);
        D3DKMT_ESCAPE escape = {};
        escape.Type = D3DKMT_ESCAPE_UPDATE_RESOURCE_WINE;
        escape.pPrivateDriverData = name;
        escape.PrivateDriverDataSize = sizeof(name);
        escape.hContext = local_kmt;
        status = D3DKMTEscape(&escape); // NTSTATUS: 0 is success
        NoteFenceKmt("D3DKMTEscape", status, "shared fence, share key " + std::to_string(share_key));
      }
    }
  }

  if (!why.empty()) {
    if (share_key) {
      WMTShareRelease(share_key);
      share_key = 0;
    }
    if (local_kmt) {
      D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
      destroy.hSyncObject = local_kmt;
      D3DKMTDestroySynchronizationObject(&destroy);
      local_kmt = 0;
    }
    share::Note("shared fence not shareable", true, "[d3d11] shared fence -> not shareable (" + why + ")");
    return;
  }
  share::Note(
      "shared fence", false,
      "[d3d11] shared fence -> sync object " + share::Hex(local_kmt) + ", share key " + std::to_string(share_key) +
          " (an NT handle comes from CreateSharedHandle)"
  );
}
#endif

HRESULT
CreateFence(MTLD3D11Device *pDevice, UINT64 InitialValue, D3D11_FENCE_FLAG Flags, REFIID riid, void **ppFence) {
  bool shared = !!(Flags & (D3D11_FENCE_FLAG_SHARED | D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER));
  auto event = pDevice->GetMTLDevice().newSharedEvent();
  D3DKMT_HANDLE local_kmt = 0;
  uint32_t share_key = 0; // MYIOSDECK spec 05 item 91 (iOS)
#ifdef DXMT_IOS
  if (shared)
    ShareFenceIOS(pDevice, event, local_kmt, share_key);
#else
  if (shared) {
    if (!(pDevice->GetLocalD3DKMT() & 0xc0000000)) {
      ERR("D3D11Fence: Invalid device handle");
      return E_FAIL;
    }
    D3DKMT_CREATESYNCHRONIZATIONOBJECT2 create = {};
    create.hDevice = pDevice->GetLocalD3DKMT();
    create.Info.Type = D3DDDI_FENCE;
    create.Info.Flags.Shared = 1;
    create.Info.Flags.NtSecuritySharing = 1;
    if (D3DKMTCreateSynchronizationObject2(&create)) {
      ERR("D3D11Fence: Failed to create D3DKMT handle");
      return E_FAIL;
    }
    local_kmt = create.hSyncObject;

    mach_port_t mach_port = event.createMachPort();
    if (!mach_port) {
      ERR("D3D11Fence: Failed to create mach port for shared fence");
      return E_FAIL;
    }
    char mach_port_name[54];
    MakeUniqueSharedName(mach_port_name);
    if (!WMTBootstrapRegister(mach_port_name, mach_port)) {
      ERR("D3D11Fence: Failed to register mach port for shared fence");
      return E_FAIL;
    }
    D3DKMT_ESCAPE escape = {};
    escape.Type = D3DKMT_ESCAPE_UPDATE_RESOURCE_WINE;
    escape.pPrivateDriverData = mach_port_name;
    escape.PrivateDriverDataSize = sizeof(mach_port_name);
    escape.hContext = local_kmt;
    if (!D3DKMTEscape(&escape)) {
      ERR("D3D11Fence: Failed to escape mach port for shared fence");
      return E_FAIL;
    }
  }
#endif
  event.signalValue(InitialValue);
  // MYIOSDECK spec 05 item 91: held, so a refused interface frees the fence (and its share key)
  Com<MTLD3D11FenceImpl> fence = ref(new MTLD3D11FenceImpl(pDevice, std::move(event), local_kmt, share_key));
  return fence->QueryInterface(riid, ppFence);
}

HRESULT
OpenSharedFence(MTLD3D11Device *pDevice, HANDLE hResource,
                REFIID riid, void **ppFence) {
  InitReturnPtr(ppFence);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("OpenSharedFence: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppFence == nullptr)
    return S_FALSE;

  char mach_port_name[54];

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = mach_port_name;
  query.PrivateRuntimeDataSize = sizeof(mach_port_name);

  NTSTATUS status = D3DKMTQueryResourceInfoFromNtHandle(&query);
#ifdef DXMT_IOS
  /* MYIOSDECK spec 05 item 91: each step logged, and a refusal says why */
  const std::string handle = "NT handle " + str::format(hResource);
  NoteFenceKmt("D3DKMTQueryResourceInfoFromNtHandle", status, "fence, " + handle);
  auto missing = [&](const std::string &why) {
    share::Note("open shared fence missing", true, share::OpenLine("fence", handle, "missing", why));
  };
  if (status)
    missing("no runtime data behind the handle");
#endif
  if (status) {
    WARN(str::format("OpenSharedFence: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }

  if (query.PrivateRuntimeDataSize != sizeof(mach_port_name)) {
#ifdef DXMT_IOS
    missing("runtime data of " + std::to_string(query.PrivateRuntimeDataSize) + " bytes");
#endif
    WARN(str::format("OpenSharedFence: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 open = {};
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;

  status = D3DKMTOpenSyncObjectFromNtHandle2(&open);
#ifdef DXMT_IOS
  NoteFenceKmt("D3DKMTOpenSyncObjectFromNtHandle2", status, "fence, " + handle);
  if (status)
    missing("no sync object behind the handle");
#endif
  if (status) {
    WARN(str::format("OpenSharedFence: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

#ifdef DXMT_IOS
  /* MYIOSDECK spec 05 item 91: the event in the share table; on a refusal the sync object just
     opened is closed again */
  auto close_sync = [&]() {
    D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
    destroy.hSyncObject = open.hSyncObject;
    D3DKMTDestroySynchronizationObject(&destroy);
  };
  uint32_t key = 0;
  if (!share::ParseShareName(mach_port_name, sizeof(mach_port_name), &key)) {
    close_sync();
    missing("no share key in its runtime data");
    return E_INVALIDARG;
  }
  bool wrong_kind = false;
  obj_handle_t shared_event = WMTOpenSharedEvent(key, &wrong_kind);
  if (!shared_event) {
    close_sync();
    missing("share key " + std::to_string(key) + (wrong_kind ? " is a texture's" : " is in no entry"));
    return E_INVALIDARG;
  }
  share::Note(
      "open shared fence found", false,
      share::OpenLine("fence", handle, "found", "share key " + std::to_string(key))
  );
  Com<MTLD3D11FenceImpl> opened = ref(new MTLD3D11FenceImpl(
      pDevice, WMT::Reference<WMT::SharedEvent>(shared_event), open.hSyncObject, key
  ));
  return opened->QueryInterface(riid, ppFence);
#else
  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    return E_INVALIDARG;
  }

  auto fence = new MTLD3D11FenceImpl(
      pDevice,
      pDevice->GetMTLDevice().newSharedEventWithMachPort(mach_port),
      open.hSyncObject);
  return fence->QueryInterface(riid, ppFence);
#endif
}

} // namespace dxmt