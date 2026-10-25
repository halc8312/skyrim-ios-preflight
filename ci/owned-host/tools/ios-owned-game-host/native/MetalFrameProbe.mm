#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <vector>
#include "MetalFrameProbe.h"
#include "FramePlanPacket.h"
#include "SampledFramePacket.h"
#include "TargetFramePacket.h"
#include "TargetFrameLifetime.h"
#include "public-shaders/PublicFrameShaders.h"
#include "public-sampled-shaders/PublicSampledShaders.h"
#if !__has_feature(objc_arc)
#error ARC required for completion-owned GPU objects
#endif

#if defined(OWNED_HOST_SDK_TRACE_TEST)
// Keep the Rust test entry in the host executable, independently of CGU/archive
// placement. It is called only by the public XCTest bundle, never at launch.
__attribute__((used)) static auto frame_test_entry = &OHRunPublicFrameComparisonV1;
__attribute__((used)) static auto sampled_frame_test_entry = &OHRunPublicSampledFrameComparisonV1;
__attribute__((used)) static auto target_frame_test_entry = &OHRunPublicTargetFrameComparisonV1;
#endif

namespace {
struct Context {
    bool sampled=false;
    pthread_t owner=pthread_self();
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLLibrary> vertexLibrary,pixelLibrary;
    id<MTLFunction> vertex,pixel;
    std::array<id<MTLRenderPipelineState>,16> pipelines{};
    std::array<id<MTLDepthStencilState>,2> depth{};
    bool owns() const {return pthread_equal(owner,pthread_self());}
    id<MTLRenderPipelineState> pipeline(uint32_t flags,uint32_t topology) {
        // Depth compare/write live in MTLDepthStencilState, not in this PSO.
        auto key=(flags&1)|(topology<<2);
        if(pipelines[key])return pipelines[key];
        MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];
        d.vertexFunction=vertex;d.fragmentFunction=pixel;d.rasterSampleCount=1;d.alphaToCoverageEnabled=NO;
        d.inputPrimitiveTopology=MTLPrimitiveTopologyClassTriangle;d.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;
        auto color=d.colorAttachments[0];color.pixelFormat=MTLPixelFormatRGBA8Unorm;color.writeMask=MTLColorWriteMaskAll;
        color.blendingEnabled=(flags&1)!=0;color.rgbBlendOperation=MTLBlendOperationAdd;color.alphaBlendOperation=MTLBlendOperationAdd;
        color.sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;color.destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        color.sourceAlphaBlendFactor=MTLBlendFactorOne;color.destinationAlphaBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
        NSError *error=nil;auto p=[device newRenderPipelineStateWithDescriptor:d error:&error];
        if(!p)fprintf(stderr,"PUBLIC_FRAME_PIPELINE_ERROR %s\n",error.description.UTF8String ?: "unknown");
        pipelines[key]=p;return p;
    }
};

bool check_sampled(const ohsample::Packet &p,uint32_t draw) {
    for(auto b:ohsample::pixel_samples)if(b.used){auto a=p.find_binding(draw,b.binding);
        if(a==SIZE_MAX||p.wire.u32(a+8)!=b.kind)return false;}
    for(uint32_t i=0;i<p.word(9);++i){auto a=p.binding(i);if(p.wire.u32(a)!=draw)continue;
        bool found=false;for(auto b:ohsample::pixel_samples)if(b.binding==p.wire.u32(a+4)&&b.kind==p.wire.u32(a+8))found=true;
        if(!found)return false;}
    return true;
}
MTLSamplerAddressMode address(uint32_t a) {
    return a==0?MTLSamplerAddressModeClampToEdge:a==1?MTLSamplerAddressModeRepeat:MTLSamplerAddressModeMirrorRepeat;
}

size_t find_binding(const ohframe::Packet &p,size_t draw,uint32_t group,uint32_t binding) {
    for(uint32_t j=0;j<p.u32(draw+92);++j){auto a=p.binding(p.u32(draw+88)+j);if(p.u32(a)==group&&p.u32(a+4)==binding)return a;}
    return SIZE_MAX;
}
template<size_t N> bool check_bindings(const ohframe::Packet &p,size_t draw,const std::array<ohframe::ShaderBinding,N> &bindings) {
    for(auto b:bindings)if(b.used){auto a=find_binding(p,draw,b.group,b.binding);
        if(a==SIZE_MAX||p.u64(a+24)<b.minimum_bytes||p.u64(a+16)%256)return false;}
    return true;
}
template<size_t N> bool known_binding(uint32_t group,uint32_t binding,const std::array<ohframe::ShaderBinding,N> &bindings) {
    for(auto b:bindings)if(b.group==group&&b.binding==binding)return true;return false;
}
template<size_t N,size_t S> void bind_stage(id<MTLRenderCommandEncoder> encoder,bool vertex,const ohframe::Packet &p,size_t draw,
    const std::vector<id<MTLBuffer>> &resources,const std::array<ohframe::ShaderBinding,N> &bindings,
    const std::array<ohframe::SizeWord,S> &sizes,int sizes_slot) {
    for(auto b:bindings)if(b.used){auto a=find_binding(p,draw,b.group,b.binding);
        if(vertex)[encoder setVertexBuffer:resources[p.u32(a+8)] offset:NSUInteger(p.u64(a+16)) atIndex:b.slot];
        else [encoder setFragmentBuffer:resources[p.u32(a+8)] offset:NSUInteger(p.u64(a+16)) atIndex:b.slot];}
    if constexpr(S>0){
        if(sizes_slot<0)return;
        std::array<uint32_t,S> values{};
        for(auto s:sizes){auto a=find_binding(p,draw,s.group,s.binding);if(a!=SIZE_MAX)values[s.word]=uint32_t(p.u64(a+24));}
        if(vertex)[encoder setVertexBytes:values.data() length:sizeof(values) atIndex:NSUInteger(sizes_slot)];
        else [encoder setFragmentBytes:values.data() length:sizeof(values) atIndex:NSUInteger(sizes_slot)];
    }
}
}

static void *create_frame(bool sampled) {
    @autoreleasepool {try {
        auto c=std::make_unique<Context>();c->sampled=sampled;c->device=MTLCreateSystemDefaultDevice();if(!c->device)return nullptr;c->queue=[c->device newCommandQueue];
        NSBundle *bundle=[NSBundle mainBundle];
        NSURL *v=[bundle URLForResource:@"PublicXenosVertex" withExtension:@"metallib"],*f=[bundle URLForResource:sampled?@"PublicSampledPixel":@"PublicXenosPixel" withExtension:@"metallib"];
        if(!v||!f){fprintf(stderr,"PUBLIC_FRAME_LIBRARY_ERROR missing offline library\n");return nullptr;}
        NSError *vertexError=nil,*pixelError=nil;
        c->vertexLibrary=[c->device newLibraryWithURL:v error:&vertexError];
        c->pixelLibrary=[c->device newLibraryWithURL:f error:&pixelError];
        if(!c->vertexLibrary||!c->pixelLibrary){fprintf(stderr,"PUBLIC_FRAME_LIBRARY_ERROR vertex=%s pixel=%s\n",vertexError.description.UTF8String ?: "none",pixelError.description.UTF8String ?: "none");return nullptr;}
        c->vertex=[c->vertexLibrary newFunctionWithName:[NSString stringWithUTF8String:ohframe::vertex_entry]];
        c->pixel=[c->pixelLibrary newFunctionWithName:[NSString stringWithUTF8String:sampled?ohsample::pixel_entry:ohframe::pixel_entry]];
        if(!c->vertex||!c->pixel){fprintf(stderr,"PUBLIC_FRAME_FUNCTION_ERROR vertex=%s pixel=%s\n",ohframe::vertex_entry,ohframe::pixel_entry);return nullptr;}
        for(unsigned i=0;i<2;++i){MTLDepthStencilDescriptor *d=[MTLDepthStencilDescriptor new];d.depthCompareFunction=i?MTLCompareFunctionLess:MTLCompareFunctionAlways;d.depthWriteEnabled=i!=0;c->depth[i]=[c->device newDepthStencilStateWithDescriptor:d];}
        if(!c->queue||!c->vertex||!c->pixel||!c->depth[0]||!c->depth[1])return nullptr;return c.release();
    } catch(...){return nullptr;}}
}
extern "C" void *OHMetalFrameCreateV1(void) {return create_frame(false);}
extern "C" void *OHMetalSampledFrameCreateV1(void) {return create_frame(true);}
extern "C" int32_t OHMetalFrameDestroyV1(void *raw){auto c=static_cast<Context *>(raw);if(!c||!c->owns())return -1;delete c;return 0;}

static int32_t render_frame(void *raw,const uint8_t *packet,size_t count,uint8_t *output,size_t output_count,const ohsample::Packet *sampled){
    auto c=static_cast<Context *>(raw);if(!c||!c->owns()||c->sampled!=(sampled!=nullptr)||!packet||!output||count<64||count>(4u<<20))return -1;
    @autoreleasepool {try {
        const ohframe::Packet p{{packet,count}};if(!p.validate()||output_count!=uint64_t(p.word(3))*p.word(4)*4)return -1;
        const uintptr_t a=reinterpret_cast<uintptr_t>(packet),b=reinterpret_cast<uintptr_t>(output);
        if(a>UINTPTR_MAX-count||b>UINTPTR_MAX-output_count||(a<b+output_count&&b<a+count))return -1;
        // Verify shader ABI before making GPU objects or recording commands.
        for(uint32_t i=0;i<p.word(7);++i){auto d=p.draw(i);
            if(!check_bindings(p,d,ohframe::vertex_bindings)
                ||(sampled?!check_bindings(p,d,ohsample::pixel_buffers):!check_bindings(p,d,ohframe::pixel_bindings))
                ||(sampled&&!check_sampled(*sampled,i)))return -1;
            for(uint32_t j=0;j<p.u32(d+92);++j){auto a=p.binding(p.u32(d+88)+j);
                if(!known_binding(p.u32(a),p.u32(a+4),ohframe::vertex_bindings)
                    &&!(sampled?known_binding(p.u32(a),p.u32(a+4),ohsample::pixel_buffers):known_binding(p.u32(a),p.u32(a+4),ohframe::pixel_bindings)))return -1;}
        }
        for(uint32_t i=0;i<p.word(7);++i){auto d=p.draw(i);if(!c->pipeline(p.u32(d+96),p.u32(d+80)))return -2;}
        std::vector<id<MTLBuffer>> resources;resources.reserve(p.word(5));
        for(uint32_t i=0;i<p.word(5);++i){auto r=p.resource(i);auto buffer=[c->device newBufferWithBytes:packet+p.u32(r+16) length:p.u32(r+20) options:MTLResourceStorageModeShared];if(!buffer)return -2;resources.push_back(buffer);}
        // Each texture/sampler generation gets a separate object, retained through
        // command completion. No replaceRegion touches an earlier version.
        std::vector<id<MTLTexture>> sampledTextures;
        std::vector<id<MTLSamplerState>> sampledSamplers;
        if(sampled){const auto &s=*sampled;const auto &w=s.wire;
            for(uint32_t i=0;i<s.word(6);++i){auto t=s.texture(i);const auto width=w.u32(t+16),height=w.u32(t+20),levels=w.u32(t+28);
                auto td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
                td.mipmapLevelCount=levels;td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
                auto texture=[c->device newTextureWithDescriptor:td];if(!texture)return -2;
                for(uint32_t level=0;level<levels;++level){auto m=s.mip(w.u32(t+24)+level);
                    [texture replaceRegion:MTLRegionMake2D(0,0,std::max(1u,width>>level),std::max(1u,height>>level)) mipmapLevel:level withBytes:w.bytes.data()+w.u32(m+12) bytesPerRow:w.u32(m+8)];}
                sampledTextures.push_back(texture);
            }
            for(uint32_t i=0;i<s.word(8);++i){auto a=s.sampler(i);auto sd=[MTLSamplerDescriptor new];sd.normalizedCoordinates=YES;
                sd.sAddressMode=address(w.u32(a+16));sd.tAddressMode=address(w.u32(a+20));sd.rAddressMode=address(w.u32(a+24));
                sd.minFilter=w.u32(a+28)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
                sd.magFilter=w.u32(a+32)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
                sd.mipFilter=w.u32(a+36)?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;
                sd.lodMinClamp=w.f32(a+40);sd.lodMaxClamp=w.f32(a+44);sd.maxAnisotropy=w.u32(a+48);
                auto sampler=[c->device newSamplerStateWithDescriptor:sd];if(!sampler)return -2;sampledSamplers.push_back(sampler);
            }
        }
        auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:p.word(3) height:p.word(4) mipmapped:NO];
        desc.storageMode=MTLStorageModePrivate;desc.usage=MTLTextureUsageRenderTarget;auto color=[c->device newTextureWithDescriptor:desc];
        desc.pixelFormat=MTLPixelFormatDepth32Float;auto depth=[c->device newTextureWithDescriptor:desc];
        const size_t row=(p.word(3)*4+255)&~size_t(255);auto read=[c->device newBufferWithLength:row*p.word(4) options:MTLResourceStorageModeShared];
        if(!color||!depth||!read)return -2;memset(read.contents,0x5a,read.length);
        auto commands=[c->queue commandBuffer];if(!commands)return -2;
        for(uint32_t i=0;i<p.word(6);++i){auto pass=p.pass(i);auto flags=p.u32(pass+48);MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
            auto ca=rp.colorAttachments[0];ca.texture=color;ca.loadAction=flags&1?MTLLoadActionClear:MTLLoadActionLoad;ca.storeAction=MTLStoreActionStore;
            ca.clearColor=MTLClearColorMake(p.f64(pass),p.f64(pass+8),p.f64(pass+16),p.f64(pass+24));
            rp.depthAttachment.texture=depth;rp.depthAttachment.loadAction=flags&2?MTLLoadActionClear:MTLLoadActionLoad;rp.depthAttachment.storeAction=MTLStoreActionStore;rp.depthAttachment.clearDepth=p.f64(pass+32);
            auto encoder=[commands renderCommandEncoderWithDescriptor:rp];if(!encoder)return -2;
            [encoder setCullMode:MTLCullModeNone];[encoder setFrontFacingWinding:MTLWindingCounterClockwise];[encoder setDepthClipMode:MTLDepthClipModeClip];
            for(uint32_t j=0;j<p.u32(pass+44);++j){auto d=p.draw(p.u32(pass+40)+j);auto flags=p.u32(d+96),topology=p.u32(d+80);
                [encoder setRenderPipelineState:c->pipeline(flags,topology)];[encoder setDepthStencilState:c->depth[(flags>>1)&1]];
                bind_stage(encoder,true,p,d,resources,ohframe::vertex_bindings,ohframe::vertex_sizes,ohframe::vertex_sizes_slot);
                if(sampled){
                    bind_stage(encoder,false,p,d,resources,ohsample::pixel_buffers,ohsample::pixel_sizes,ohsample::pixel_sizes_slot);
                    for(auto b:ohsample::pixel_samples)if(b.used){auto a=sampled->find_binding(p.u32(pass+40)+j,b.binding);auto index=sampled->wire.u32(a+12);
                        if(b.kind==1)[encoder setFragmentTexture:sampledTextures[index] atIndex:b.slot];
                        else [encoder setFragmentSamplerState:sampledSamplers[index] atIndex:b.slot];}
                }else bind_stage(encoder,false,p,d,resources,ohframe::pixel_bindings,ohframe::pixel_sizes,ohframe::pixel_sizes_slot);
                [encoder setViewport:MTLViewport{p.f32(d),p.f32(d+4),p.f32(d+8),p.f32(d+12),p.f32(d+16),p.f32(d+20)}];
                [encoder setScissorRect:MTLScissorRect{p.u32(d+24),p.u32(d+28),p.u32(d+32),p.u32(d+36)}];
                [encoder setBlendColorRed:float(p.f64(d+40)) green:float(p.f64(d+48)) blue:float(p.f64(d+56)) alpha:float(p.f64(d+64))];[encoder setStencilReferenceValue:p.u32(d+72)];
                auto primitive=topology==1?MTLPrimitiveTypeTriangle:MTLPrimitiveTypeTriangleStrip;
                if(auto index=p.u32(d+100)) [encoder drawIndexedPrimitives:primitive indexCount:p.u32(d+84) indexType:p.u32(d+104)==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32 indexBuffer:resources[index-1] indexBufferOffset:p.u32(d+108)];
                else [encoder drawPrimitives:primitive vertexStart:0 vertexCount:p.u32(d+84)];
            }
            [encoder endEncoding];
        }
        auto blit=[commands blitCommandEncoder];if(!blit)return -2;
        [blit copyFromTexture:color sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(p.word(3),p.word(4),1) toBuffer:read destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*p.word(4)];[blit endEncoding];
        [commands commit];[commands waitUntilCompleted];if(commands.status!=MTLCommandBufferStatusCompleted||commands.error)return -3;
        for(uint32_t y=0;y<p.word(4);++y)memcpy(output+y*p.word(3)*4,static_cast<const uint8_t *>(read.contents)+y*row,p.word(3)*4);
        return 0;
    }catch(...){return -4;}}
}
extern "C" int32_t OHMetalFrameRenderV1(void *raw,const uint8_t *packet,size_t count,uint8_t *output,size_t output_count){
    return render_frame(raw,packet,count,output,output_count,nullptr);
}
extern "C" int32_t OHMetalSampledFrameRenderV1(void *raw,const uint8_t *packet,size_t count,uint8_t *output,size_t output_count){
    auto c=static_cast<Context *>(raw);
    if(!c||!c->owns()||!c->sampled||!packet||!output||count<80||count>(4u<<20))return -1;
    try {
        const uintptr_t a=reinterpret_cast<uintptr_t>(packet),b=reinterpret_cast<uintptr_t>(output);
        if(a>UINTPTR_MAX-count||b>UINTPTR_MAX-output_count||(a<b+output_count&&b<a+count))return -1;
        const ohsample::Packet s{{{packet,count}}};if(!s.validate())return -1;
        const auto p=s.geometry();return render_frame(raw,p.bytes.data(),p.bytes.size(),output,output_count,&s);
    }catch(...){return -4;}
}

// Separate OHRT executor: old OHFP/OHST execution above is kept intact. Both use
// the same compiled shader ABI, PSOs, draw-state binding and same-thread context.
extern "C" void *OHMetalTargetFrameCreateV1(void){return create_frame(true);}
static int32_t render_targets(void *raw,const uint8_t *packet,size_t count,uint8_t *output,size_t output_count,bool reuse,OHFrameAllocationV1 *audit=nullptr){
    auto c=static_cast<Context *>(raw);
    if(!c||!c->owns()||!c->sampled||!packet||!output||count<80||count>(4u<<20))return -1;
    const uintptr_t a=reinterpret_cast<uintptr_t>(packet),b=reinterpret_cast<uintptr_t>(output);
    if(a>UINTPTR_MAX-count||b>UINTPTR_MAX-output_count||(a<b+output_count&&b<a+count))return -1;
    @autoreleasepool {try{
        const ohtarget::Packet targets{{{packet,count}}};if(!targets.validate())return -1;
        const auto allocation=ohtarget::Allocation::make(targets,reuse);if(!allocation)return -1;
        const auto input=targets.inputs();const auto p=input.geometry();const auto &w=input.wire;const auto &t=targets.wire;
        const auto result=targets.target(targets.word(9));const uint32_t width=t.u32(result+16),height=t.u32(result+20);
        if(output_count!=uint64_t(width)*height*4)return -1;
        for(uint32_t i=0;i<p.word(7);++i){auto d=p.draw(i);
            if(!check_bindings(p,d,ohframe::vertex_bindings)||!check_bindings(p,d,ohsample::pixel_buffers))return -1;
            for(uint32_t j=0;j<p.u32(d+92);++j){auto x=p.binding(p.u32(d+88)+j);
                if(!known_binding(p.u32(x),p.u32(x+4),ohframe::vertex_bindings)&&!known_binding(p.u32(x),p.u32(x+4),ohsample::pixel_buffers))return -1;}
            for(auto binding:ohsample::pixel_samples)if(binding.used){auto x=targets.find_binding(i,binding.binding);
                if(x==SIZE_MAX)return -1;auto kind=t.u32(x+8);
                if(binding.kind==1?(kind!=1&&kind!=3):kind!=2)return -1;}
            for(uint32_t j=0;j<targets.word(8);++j){auto x=targets.binding(j);if(t.u32(x)!=i)continue;
                bool known=false;for(auto binding:ohsample::pixel_samples)
                    if(binding.binding==t.u32(x+4)&&(binding.kind==2?t.u32(x+8)==2:t.u32(x+8)==1||t.u32(x+8)==3))known=true;
                if(!known)return -1;}
            if(!c->pipeline(p.u32(d+96),p.u32(d+80)))return -2;
        }
        std::vector<id<MTLBuffer>> buffers;
        for(uint32_t i=0;i<p.word(5);++i){auto x=p.resource(i);
            auto buffer=[c->device newBufferWithBytes:p.bytes.data()+p.u32(x+16) length:p.u32(x+20) options:MTLResourceStorageModeShared];
            if(!buffer)return -2;buffers.push_back(buffer);}
        std::vector<id<MTLTexture>> inputs,images;std::vector<id<MTLSamplerState>> samplers;
        for(uint32_t i=0;i<input.word(6);++i){auto x=input.texture(i);auto iw=w.u32(x+16),ih=w.u32(x+20),levels=w.u32(x+28);
            auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:iw height:ih mipmapped:NO];
            desc.mipmapLevelCount=levels;desc.storageMode=MTLStorageModeShared;desc.usage=MTLTextureUsageShaderRead;
            auto image=[c->device newTextureWithDescriptor:desc];if(!image)return -2;
            for(uint32_t level=0;level<levels;++level){auto m=input.mip(w.u32(x+24)+level);
                [image replaceRegion:MTLRegionMake2D(0,0,std::max(1u,iw>>level),std::max(1u,ih>>level)) mipmapLevel:level
                    withBytes:w.bytes.data()+w.u32(m+12) bytesPerRow:w.u32(m+8)];}
            inputs.push_back(image);
        }
        for(uint32_t i=0;i<input.word(8);++i){auto x=input.sampler(i);auto desc=[MTLSamplerDescriptor new];desc.normalizedCoordinates=YES;
            desc.sAddressMode=address(w.u32(x+16));desc.tAddressMode=address(w.u32(x+20));desc.rAddressMode=address(w.u32(x+24));
            desc.minFilter=w.u32(x+28)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
            desc.magFilter=w.u32(x+32)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
            desc.mipFilter=w.u32(x+36)?MTLSamplerMipFilterLinear:MTLSamplerMipFilterNearest;
            desc.lodMinClamp=w.f32(x+40);desc.lodMaxClamp=w.f32(x+44);desc.maxAnisotropy=w.u32(x+48);
            auto sampler=[c->device newSamplerStateWithDescriptor:desc];if(!sampler)return -2;samplers.push_back(sampler);
        }
        std::vector<id<MTLTexture>> slots;
        for(auto slot:allocation->slots){const bool color=slot.kind==1;
            auto desc=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:color?MTLPixelFormatRGBA8Unorm:MTLPixelFormatDepth32Float
                width:slot.width height:slot.height mipmapped:NO];
            desc.storageMode=MTLStorageModePrivate;desc.hazardTrackingMode=MTLHazardTrackingModeTracked;
            desc.usage=color?(MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead):MTLTextureUsageRenderTarget;
            auto image=[c->device newTextureWithDescriptor:desc];if(!image)return -2;slots.push_back(image);
        }
        for(auto slot:allocation->mapping)images.push_back(slots[slot]);
        const uint32_t row=(width*4+255)&~255u;
        auto read=[c->device newBufferWithLength:uint64_t(row)*height options:MTLResourceStorageModeShared];
        auto commands=[c->queue commandBuffer];if(!read||!commands)return -2;
        auto copy=[&](id<MTLTexture> source,uint32_t level,uint32_t x,uint32_t y,id<MTLTexture> destination){
            auto blit=[commands blitCommandEncoder];if(!blit)return false;
            [blit copyFromTexture:source sourceSlice:0 sourceLevel:level sourceOrigin:MTLOriginMake(x,y,0)
                sourceSize:MTLSizeMake(destination.width,destination.height,1) toTexture:destination destinationSlice:0
                destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];[blit endEncoding];return true;
        };
        // Aliased generations have disjoint proven lifetimes. Tracked textures,
        // encoder boundaries and one command buffer preserve all hazards; every
        // strong slot reference stays alive through waitUntilCompleted below.
        for(uint32_t oi=0;oi<targets.word(7);++oi){auto op=targets.operation(oi);
            if(t.u32(op)==2){auto source=t.u32(op+4)==1?inputs[t.u32(op+8)]:images[t.u32(op+8)];
                if(allocation->copy_elided[oi])continue;
                if(!copy(source,t.u32(op+12),t.u32(op+20),t.u32(op+24),images[t.u32(op+16)]))return -2;continue;}
            auto color=images[t.u32(op+8)],depth=images[t.u32(op+12)];
            if(auto load=t.u32(op+16))if(!allocation->load_elided[oi][0]&&!copy(images[load-1],0,0,0,color))return -2;
            if(auto load=t.u32(op+20))if(!allocation->load_elided[oi][1]&&!copy(images[load-1],0,0,0,depth))return -2;
            auto pass=p.pass(t.u32(op+4));auto flags=p.u32(pass+48),stores=t.u32(op+24);
            auto desc=[MTLRenderPassDescriptor renderPassDescriptor];desc.colorAttachments[0].texture=color;
            desc.colorAttachments[0].loadAction=flags&1?MTLLoadActionClear:MTLLoadActionLoad;
            desc.colorAttachments[0].storeAction=stores&1?MTLStoreActionStore:MTLStoreActionDontCare;
            desc.colorAttachments[0].clearColor=MTLClearColorMake(p.f64(pass),p.f64(pass+8),p.f64(pass+16),p.f64(pass+24));
            desc.depthAttachment.texture=depth;desc.depthAttachment.loadAction=flags&2?MTLLoadActionClear:MTLLoadActionLoad;
            desc.depthAttachment.storeAction=stores&2?MTLStoreActionStore:MTLStoreActionDontCare;desc.depthAttachment.clearDepth=p.f64(pass+32);
            auto encoder=[commands renderCommandEncoderWithDescriptor:desc];if(!encoder)return -2;
            [encoder setCullMode:MTLCullModeNone];[encoder setFrontFacingWinding:MTLWindingCounterClockwise];[encoder setDepthClipMode:MTLDepthClipModeClip];
            for(uint32_t j=0;j<p.u32(pass+44);++j){auto index=p.u32(pass+40)+j;auto d=p.draw(index);auto state=p.u32(d+96),topology=p.u32(d+80);
                [encoder setRenderPipelineState:c->pipeline(state,topology)];[encoder setDepthStencilState:c->depth[(state>>1)&1]];
                bind_stage(encoder,true,p,d,buffers,ohframe::vertex_bindings,ohframe::vertex_sizes,ohframe::vertex_sizes_slot);
                bind_stage(encoder,false,p,d,buffers,ohsample::pixel_buffers,ohsample::pixel_sizes,ohsample::pixel_sizes_slot);
                for(auto binding:ohsample::pixel_samples)if(binding.used){auto x=targets.find_binding(index,binding.binding);auto ri=t.u32(x+12),kind=t.u32(x+8);
                    if(kind==2)[encoder setFragmentSamplerState:samplers[ri] atIndex:binding.slot];
                    else [encoder setFragmentTexture:kind==1?inputs[ri]:images[ri] atIndex:binding.slot];}
                [encoder setViewport:MTLViewport{p.f32(d),p.f32(d+4),p.f32(d+8),p.f32(d+12),p.f32(d+16),p.f32(d+20)}];
                [encoder setScissorRect:MTLScissorRect{p.u32(d+24),p.u32(d+28),p.u32(d+32),p.u32(d+36)}];
                [encoder setBlendColorRed:float(p.f64(d+40)) green:float(p.f64(d+48)) blue:float(p.f64(d+56)) alpha:float(p.f64(d+64))];
                [encoder setStencilReferenceValue:p.u32(d+72)];auto primitive=topology==1?MTLPrimitiveTypeTriangle:MTLPrimitiveTypeTriangleStrip;
                if(auto bi=p.u32(d+100))[encoder drawIndexedPrimitives:primitive indexCount:p.u32(d+84) indexType:p.u32(d+104)==2?MTLIndexTypeUInt16:MTLIndexTypeUInt32
                    indexBuffer:buffers[bi-1] indexBufferOffset:p.u32(d+108)];
                else [encoder drawPrimitives:primitive vertexStart:0 vertexCount:p.u32(d+84)];
            }
            [encoder endEncoding];
        }
        auto blit=[commands blitCommandEncoder];if(!blit)return -2;
        [blit copyFromTexture:images[targets.word(9)] sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(width,height,1)
            toBuffer:read destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*height];[blit endEncoding];
        [commands commit];[commands waitUntilCompleted];if(commands.status!=MTLCommandBufferStatusCompleted||commands.error)return -3;
        OHFrameAllocationV1 actual{};
        if(audit){
            actual.version=0x10000;actual.bytes=sizeof(actual);actual.policy=reuse?1:0;
            actual.slots=uint32_t(slots.size());actual.targets=uint32_t(images.size());actual.operations=targets.word(7);
            actual.dedicated_bytes=allocation->dedicated_bytes;actual.allocated_bytes=allocation->allocated_bytes();
            for(uint32_t i=0;i<actual.targets;++i)actual.mapping[i]=allocation->mapping[i];
            for(uint32_t i=0;i<actual.operations;++i){
                if(allocation->load_elided[i][0])actual.color_load_elided|=uint64_t(1)<<i;
                if(allocation->load_elided[i][1])actual.depth_load_elided|=uint64_t(1)<<i;
                if(allocation->copy_elided[i])actual.copy_elided|=uint64_t(1)<<i;
            }
        }
        for(uint32_t y=0;y<height;++y)memcpy(output+y*width*4,static_cast<const uint8_t*>(read.contents)+y*row,width*4);
        if(audit)*audit=actual;
        return 0;
    }catch(...){return -4;}}
}
extern "C" int32_t OHMetalTargetFrameRenderV1(void *raw,const uint8_t *packet,size_t count,uint8_t *output,size_t output_count){
    return render_targets(raw,packet,count,output,output_count,false);
}
extern "C" int32_t OHMetalTargetFrameRenderReusingV1(void *raw,const uint8_t *packet,size_t count,uint8_t *output,size_t output_count){
    return render_targets(raw,packet,count,output,output_count,true);
}
extern "C" int32_t OHMetalTargetFrameRenderAuditedV1(void *raw,const uint8_t *packet,size_t count,uint32_t policy,
    uint8_t *output,size_t output_count,OHFrameAllocationV1 *audit,size_t audit_bytes){
    if(policy>1||!audit||audit_bytes!=sizeof(*audit)||reinterpret_cast<uintptr_t>(audit)%alignof(OHFrameAllocationV1))return -1;
    const uintptr_t a=reinterpret_cast<uintptr_t>(packet),b=reinterpret_cast<uintptr_t>(output),c=reinterpret_cast<uintptr_t>(audit);
    if(a>UINTPTR_MAX-count||b>UINTPTR_MAX-output_count||c>UINTPTR_MAX-audit_bytes
        ||(a<c+audit_bytes&&c<a+count)||(b<c+audit_bytes&&c<b+output_count))return -1;
    return render_targets(raw,packet,count,output,output_count,policy==1,audit);
}
