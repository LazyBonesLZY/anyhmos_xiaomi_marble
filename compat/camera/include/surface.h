/* SPDX-License-Identifier: MIT */
/* Minimal ABI-exact declarations matching donor libsurface.z.so. */
#ifndef MARBLE_CAMERA_SURFACE_SHIM_H
#define MARBLE_CAMERA_SURFACE_SHIM_H

#include <cstdint>
#include <string>
#include <refbase.h>
#include <native_window/buffer_handle.h>

namespace OHOS {

class MessageParcel;

class IBufferProducer : public virtual RefBase {
public:
    virtual ~IBufferProducer() = default;
};

/*
 * ABI-exact vtable layout verified against donor libsurface.z.so
 * (OHOS::BufferExtraDataImpl vtable @ 0x99588, size=160):
 *   +0/+8   ~BufferExtraData()
 *   +16..56 RefBase virtuals (6 slots)
 *   +64     ReadFromParcel
 *   +72     WriteToParcel
 *   +80     ExtraGet(string, int32_t&) const
 *   +88     ExtraGet(string, int64_t&) const
 *   +96     ExtraGet(string, double&) const
 *   +104    ExtraGet(string, string&) const
 *   +112    ExtraSet(string, int32_t)
 *   +120    ExtraSet(string, int64_t)
 *   +128    ExtraSet(string, double)
 *   +136    ExtraSet(string, string)
 */
class BufferExtraData : public RefBase {
public:
    ~BufferExtraData() override = default;
    virtual int32_t ReadFromParcel(MessageParcel &parcel) = 0;
    virtual int32_t WriteToParcel(MessageParcel &parcel) = 0;
    virtual int32_t ExtraGet(const std::string &key, int32_t &value) const = 0;
    virtual int32_t ExtraGet(const std::string &key, int64_t &value) const = 0;
    virtual int32_t ExtraGet(const std::string &key, double &value) const = 0;
    virtual int32_t ExtraGet(const std::string &key, std::string &value) const = 0;
    virtual int32_t ExtraSet(const std::string &key, int32_t value) = 0;
    virtual int32_t ExtraSet(const std::string &key, int64_t value) = 0;
    virtual int32_t ExtraSet(const std::string &key, double value) = 0;
    virtual int32_t ExtraSet(const std::string &key, const std::string &value) = 0;
};

/*
 * ABI-exact vtable prefix verified against donor libsurface.z.so
 * (OHOS::SurfaceBufferImpl vtable @ 0x9eb38):
 *   +0/+8   ~SurfaceBuffer()
 *   +16..56 RefBase virtuals (6 slots)
 *   +64     GetBufferHandle() const
 *   +72     GetWidth() const
 *   +80     GetHeight() const
 *   +88     GetStride() const
 *   +96     GetFormat() const
 *   +104    GetUsage() const
 *   +112    GetPhyAddr() const
 *   +120    GetVirAddr()
 *   +128    GetFileDescriptor() const
 *   +136    GetSize() const
 *   +144    GetSurfaceBufferColorGamut() const
 *   +152    GetSurfaceBufferTransform() const
 *   +160    SetSurfaceBufferColorGamut(const int32_t&)
 *   +168    SetSurfaceBufferTransform(const int32_t&)
 *   +176    GetSurfaceBufferWidth() const
 *   +184    GetSurfaceBufferHeight() const
 *   +192    SetSurfaceBufferWidth(int32_t)
 *   +200    SetSurfaceBufferHeight(int32_t)
 *   +208    GetSeqNum() const
 *   +216    SetExtraData(sptr<BufferExtraData>)
 *   +224    GetExtraData() const
 */
class SurfaceBuffer : public RefBase {
public:
    ~SurfaceBuffer() override = default;
    virtual BufferHandle *GetBufferHandle() const = 0;
    virtual int32_t GetWidth() const = 0;
    virtual int32_t GetHeight() const = 0;
    virtual int32_t GetStride() const = 0;
    virtual int32_t GetFormat() const = 0;
    virtual uint64_t GetUsage() const = 0;
    virtual uint64_t GetPhyAddr() const = 0;
    virtual void *GetVirAddr() = 0;
    virtual int32_t GetFileDescriptor() const = 0;
    virtual uint32_t GetSize() const = 0;
    virtual int32_t GetSurfaceBufferColorGamut() const = 0;
    virtual int32_t GetSurfaceBufferTransform() const = 0;
    virtual void SetSurfaceBufferColorGamut(const int32_t &colorGamut) = 0;
    virtual void SetSurfaceBufferTransform(const int32_t &transform) = 0;
    virtual int32_t GetSurfaceBufferWidth() const = 0;
    virtual int32_t GetSurfaceBufferHeight() const = 0;
    virtual void SetSurfaceBufferWidth(int32_t width) = 0;
    virtual void SetSurfaceBufferHeight(int32_t height) = 0;
    virtual uint32_t GetSeqNum() const = 0;
    virtual void SetExtraData(sptr<BufferExtraData> bedata) = 0;
    virtual sptr<BufferExtraData> GetExtraData() const = 0;
};

class Surface : public RefBase {
public:
    virtual ~Surface() = default;
    static sptr<Surface> CreateSurfaceAsProducer(sptr<IBufferProducer> &producer);
    static sptr<Surface> CreateSurfaceAsConsumer(std::string name = "noname");
};

} // namespace OHOS

#endif /* MARBLE_CAMERA_SURFACE_SHIM_H */
