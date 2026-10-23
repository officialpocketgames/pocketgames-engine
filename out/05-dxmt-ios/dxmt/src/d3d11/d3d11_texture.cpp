// Changed for MYIOSDECK (spec 05 Part C, clarification 3). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "d3d11_texture.hpp"
#include "d3d11_resource.hpp"
#ifdef DXMT_IOS
#include "dxmt_bc_emulation.hpp"
#include "dxmt_format.hpp"
#include "util_log_rare.hpp"
#include <cstdlib>
#endif

namespace dxmt {

#ifdef DXMT_IOS
/* Spec 05 Part C: true if `format` is a BC format whose Metal texture is
   uncompressed; `pixel_format` receives the (logical) BC format. */
static bool
IsEmulatedBCDxgiFormat(MTLD3D11Device *pDevice, DXGI_FORMAT format, WMTPixelFormat &pixel_format) {
  MTL_DXGI_FORMAT_DESC desc;
  if (FAILED(MTLQueryDXGIFormat(pDevice->GetMTLDevice(), format, desc)))
    return false;
  pixel_format = desc.PixelFormat;
  return (desc.Flag & MTL_DXGI_FORMAT_BC) && IsEmulatedBCFormat(pDevice->GetMTLDevice(), desc.PixelFormat);
}
#endif

template <>
void InitializeTextureData(MTLD3D11Device *pDevice, WMT::Texture target,
                           const D3D11_TEXTURE3D_DESC1 &Desc,
                           const D3D11_SUBRESOURCE_DATA *subresources) {
  if (subresources == nullptr)
    return;
#ifdef DXMT_IOS
  WMTPixelFormat bc_format;
  if (IsEmulatedBCDxgiFormat(pDevice, Desc.Format, bc_format)) {
    /* Spec 05 clarification 3: decode slice by slice (each depth slice an
       independent 2D BC image, SysMemSlicePitch apart) and upload the texels. */
    uint32_t bpp = BCPhysicalBytesPerTexel(bc_format);
    for (uint32_t level = 0; level < Desc.MipLevels; level++) {
      uint32_t w = std::max(1u, Desc.Width >> level), h = std::max(1u, Desc.Height >> level);
      uint32_t d = std::max(1u, Desc.Depth >> level);
      size_t row_bytes = size_t(w) * bpp;
      void *decoded = malloc(row_bytes * h * d);
      if (!decoded) {
        WARN_RARE("BC emulation: no memory to decode 3D initial data, upload skipped");
        continue;
      }
      if (BCDecodeVolumeToPhysical(
              bc_format, subresources[level].pSysMem, subresources[level].SysMemPitch,
              subresources[level].SysMemSlicePitch, decoded, w, h, d
          ))
        target.replaceRegion({0, 0, 0}, {w, h, d}, level, 0, decoded, row_bytes, row_bytes * h);
      else
        WARN_RARE("BC emulation: 3D initial data can't be decoded, upload skipped");
      free(decoded);
    }
    return;
  }
#endif
  uint32_t width = Desc.Width;
  uint32_t height = Desc.Height;
  uint32_t depth = Desc.Depth;
  for (uint32_t level = 0; level < Desc.MipLevels; level++) {
    target.replaceRegion({0, 0, 0}, {width, height, depth}, level, 0,
                         subresources[level].pSysMem,
                         subresources[level].SysMemPitch,
                         subresources[level].SysMemSlicePitch);
    width = std::max(1u, width >> 1);
    height = std::max(1u, height >> 1);
    depth = std::max(1u, depth >> 1);
  }
};

template <>
void InitializeTextureData(MTLD3D11Device *pDevice, WMT::Texture target,
                           const D3D11_TEXTURE2D_DESC1 &Desc,
                           const D3D11_SUBRESOURCE_DATA *subresources) {
  if (subresources == nullptr)
    return;
#ifdef DXMT_IOS
  WMTPixelFormat bc_format;
  if (IsEmulatedBCDxgiFormat(pDevice, Desc.Format, bc_format)) {
    /* Decode each subresource and upload the physical texels. */
    uint32_t bpp = BCPhysicalBytesPerTexel(bc_format);
    for (uint32_t level = 0; level < Desc.MipLevels; level++) {
      uint32_t w = std::max(1u, Desc.Width >> level), h = std::max(1u, Desc.Height >> level);
      size_t row_bytes = size_t(w) * bpp;
      void *decoded = malloc(row_bytes * h);
      if (!decoded) {
        WARN_RARE("BC emulation: no memory to decode initial data, upload skipped");
        continue;
      }
      for (uint32_t slice = 0; slice < Desc.ArraySize; slice++) {
        auto idx = D3D11CalcSubresource(level, slice, Desc.MipLevels);
        if (!BCDecodeToPhysical(
                bc_format, subresources[idx].pSysMem, subresources[idx].SysMemPitch, decoded, row_bytes, w, h
            )) {
          WARN_RARE("BC emulation: initial data can't be decoded, upload skipped");
          continue;
        }
        target.replaceRegion({0, 0, 0}, {w, h, 1}, level, slice, decoded, row_bytes, 0);
      }
      free(decoded);
    }
    return;
  }
#endif
  uint32_t width = Desc.Width;
  uint32_t height = Desc.Height;
  for (uint32_t level = 0; level < Desc.MipLevels; level++) {
    uint32_t bytes_per_row = 0, _, __;
    GetLinearTextureLayout(pDevice, Desc, level, bytes_per_row, _, __, false);
    for (uint32_t slice = 0; slice < Desc.ArraySize; slice++) {
      auto idx = D3D11CalcSubresource(level, slice, Desc.MipLevels);
      if (subresources[idx].SysMemPitch < bytes_per_row) {
        WARN("RowPitch provided ", subresources[idx].SysMemPitch,
             ", expect at least ", bytes_per_row);
      }
      target.replaceRegion(
          {0, 0, 0}, {width, height, 1}, level, slice,
          subresources[idx].pSysMem,
          std::max(subresources[idx].SysMemPitch, bytes_per_row), 0);
    }
    width = std::max(1u, width >> 1);
    height = std::max(1u, height >> 1);
  }
};

template <>
void InitializeTextureData(MTLD3D11Device *pDevice, WMT::Texture target,
                           const D3D11_TEXTURE1D_DESC &Desc,
                           const D3D11_SUBRESOURCE_DATA *subresources) {
  if (subresources == nullptr)
    return;
  uint32_t width = Desc.Width;
  for (uint32_t level = 0; level < Desc.MipLevels; level++) {
    for (uint32_t slice = 0; slice < Desc.ArraySize; slice++) {
      auto idx = D3D11CalcSubresource(level, slice, Desc.MipLevels);
      uint32_t bytes_per_row = 0, _, __;
      GetLinearTextureLayout(pDevice, Desc, level, bytes_per_row, _, __, false);
      target.replaceRegion(
          {0, 0, 0}, {width, 1, 1}, level, slice, subresources[idx].pSysMem,
          std::max(subresources[idx].SysMemPitch, bytes_per_row), 0);
    }
    width = std::max(1u, width >> 1);
  }
};

} // namespace dxmt