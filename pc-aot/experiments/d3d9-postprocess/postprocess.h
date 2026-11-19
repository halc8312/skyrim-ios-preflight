/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <d3d9.h>
#include <utility>

template <class T> class PostFXRef {
  public:
    PostFXRef() = default;
    ~PostFXRef() { reset(); }
    PostFXRef(const PostFXRef&) = delete;
    PostFXRef& operator=(const PostFXRef&) = delete;
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    T** put() {
        reset();
        return &value_;
    }
    void swap(PostFXRef& other) { std::swap(value_, other.value_); }
    void reset() {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
    }

  private:
    T* value_ = nullptr;
};

// Standalone experiment. Attach one instance to one serialized device/Present
// path and release() before a device reset. The pass never calls Present.
class TwoTonePass {
  public:
    HRESULT apply(IDirect3DDevice9* device, IDirect3DSurface9* output);
    void release();

  private:
    HRESULT prepare(IDirect3DDevice9* device, const D3DSURFACE_DESC& target);
    IDirect3DDevice9* device_ = nullptr;
    D3DSURFACE_DESC cached_{};
    PostFXRef<IDirect3DTexture9> scene_;
    PostFXRef<IDirect3DSurface9> filtered_;
    PostFXRef<IDirect3DPixelShader9> shader_;
};
