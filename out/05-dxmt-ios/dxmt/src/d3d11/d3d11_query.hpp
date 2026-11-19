#pragma once
// Changed for MYIOSDECK (spec 05 clarification 63: QueryInterface counted; round 18: what a polled
// query parks on). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "com/com_guid.hpp"
#include "d3d11_device_child.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_query_poll.hpp" // MYIOSDECK spec 05 round 18
#include "log/log.hpp"
#include "../d3d10/d3d10_query.hpp"

DEFINE_COM_INTERFACE("a301e56d-d87e-4b69-8440-bd003e285904",
                     IMTLD3DOcclusionQuery)
    : public ID3D11Query {
  virtual HRESULT GetData(void *data) = 0;
  virtual bool Begin() = 0;
  virtual bool End() = 0;
  virtual void DoDeferredQuery(dxmt::Rc<dxmt::VisibilityResultQuery> &deferred_query) = 0;

  virtual dxmt::Rc<dxmt::VisibilityResultQuery> __query() = 0;
  /* MYIOSDECK spec 05 round 18 (item 80): the poll state and the chunk that holds its end */
  virtual dxmt::QueryParkInfo &parkInfo() = 0;
};

namespace dxmt {

template <typename Query>
class MTLD3DQueryBase : public MTLD3D11DeviceChild<Query> {
public:
  MTLD3DQueryBase(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC *desc)
      : MTLD3D11DeviceChild<Query>(pDevice), desc_(*desc),
        d3d10_(this, pDevice->GetImmediateContextPrivate()) {}
  ~MTLD3DQueryBase(){};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    D11_CHILD_CALL(QueryInterface); // MYIOSDECK spec 05 clarification 63
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) ||
        riid == __uuidof(ID3D11Query)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(ID3D10DeviceChild) || riid == __uuidof(ID3D10Query)) {
      *ppvObject = ref(&d3d10_);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11Query), riid)) {
      Logger::warn("D3D11Query::QueryInterface: Unknown interface query");
      Logger::warn(str::format(riid));
    }

    return E_NOINTERFACE;
  };

  void STDMETHODCALLTYPE GetDesc(D3D11_QUERY_DESC *pDesc) final {
    if (pDesc) {
      *pDesc = desc_;
    }
  };

protected:
  D3D11_QUERY_DESC desc_;
  MTLD3D10Query d3d10_;
};

template <typename DataType>
class MTLD3D11DummyQuery : public MTLD3DQueryBase<ID3D11Query> {
public:
  MTLD3D11DummyQuery(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC *desc)
      : MTLD3DQueryBase<ID3D11Query>(pDevice, desc) {}

  virtual UINT STDMETHODCALLTYPE GetDataSize() override {
    return sizeof(DataType);
  };
};

/**

From D3D11.3 Functional Spec:

- Passing IssueFlags with only the D3DISSUE_BEGIN bit set causes the Query to
enter the "building" state (regardless of whatever state it was in before)

- A second D3DISSUE_BEGIN will result in the range being reset (the first
D3DISSUE_BEGIN is effectively discarded/ ignored).

- Some Query Types only support D3DISSUE_END

- When the Query is in the "signaled" state, the Query Type supports
D3DISSUE_BEGIN, and Issue is invoked with just the D3DISSUE_END flag: it is
equivalent to invoking Issue with both D3DISSUE_BEGIN and D3DISSUE_END bits set,
as well as being equivalent to an invocation of Issue with D3DISSUE_BEGIN
followed immediately by another invocation of Issue with D3DISSUE_END

- (BEGIN and END) define a bracketing of graphics commands

- Bracketings of Queries are allowed to overlap and nest.
 */
enum class QueryState {
  /**
    After DeviceContext::Begin
   */
  Building,
  /**
    After DeviceContext::End
   */
  Issued,
  /**
    Default state, or when data is ready
   */
  Signaled,
  Undefined,
};

enum class EventState {
  Pending,
  Signaled,
  Stall,
  Invalid,
};

constexpr size_t kEventStallThreshold = 64;

struct MTLD3D11EventQuery : public ID3D11Query {
  virtual void Issue(uint64_t current_seq_id) = 0;
  /* MYIOSDECK spec 05 round 18: `count_poll` is false for item 80's second check, so that the
     64-poll stall rule still counts GetData calls */
  virtual EventState CheckEventState(uint64_t coherent_seq_id, bool count_poll = true) = 0;
  /* item 82: the shared-event value that signals it (0 before its first End) */
  virtual uint64_t SignalValue() const = 0;
  /* item 80: the poll state and the chunk that holds its signal */
  virtual QueryParkInfo &parkInfo() = 0;
};

template <typename DataType>
class MTLD3D11EventQueryImpl : public MTLD3DQueryBase<MTLD3D11EventQuery> {
public:
  MTLD3D11EventQueryImpl(MTLD3D11Device *pDevice, const D3D11_QUERY_DESC *desc)
      : MTLD3DQueryBase<MTLD3D11EventQuery>(pDevice, desc) {}

  UINT STDMETHODCALLTYPE GetDataSize() override { return sizeof(DataType); };

  void Issue(uint64_t current_seq_id) override {
    state = QueryState::Issued;
    should_be_signaled_at = current_seq_id;
    stall_counter = 0;
  }

  EventState CheckEventState(uint64_t coherent_seq_id, bool count_poll) override {
    if (state == QueryState::Undefined)
      return EventState::Invalid;
    if (should_be_signaled_at <= coherent_seq_id) {
      state = QueryState::Signaled;
      stall_counter = 0;
      return EventState::Signaled;
    }
    if (stall_counter != ~0u) {
      if (!count_poll)
        return EventState::Pending; // MYIOSDECK spec 05 round 18
      if (stall_counter < kEventStallThreshold) {
        stall_counter++;
        return EventState::Pending;
      }
      stall_counter = ~0u;
    }
    return EventState::Stall;
  };

  uint64_t SignalValue() const override { return should_be_signaled_at; }

  QueryParkInfo &parkInfo() override { return park_; }

private:
  QueryState state = QueryState::Undefined;
  uint32_t stall_counter = 0;
  uint64_t should_be_signaled_at = 0;
  QueryParkInfo park_; // MYIOSDECK spec 05 round 18 (item 80)
};

HRESULT CreateOcculusionQuery(MTLD3D11Device *pDevice,
                              const D3D11_QUERY_DESC *pDesc,
                              ID3D11Query **ppQuery);

} // namespace dxmt