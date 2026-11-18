// Authored arithmetic only. Execute the translated fragment on a build Mac;
// this does not execute Skyrim, iOS, or any private shader.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

static void require(bool condition, NSString *message) {
    if (!condition) throw std::runtime_error(message.UTF8String ?: "Metal fixture failure");
}
static id<MTLLibrary> library(id<MTLDevice> device, NSString *source, bool fast) {
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_1;
    // These correspond to MoltenVK's two relevant compile policies. Do not
    // change the app's policy merely to make this fixture pass.
    if (@available(macOS 15.0, *)) {
        options.mathMode = fast ? MTLMathModeFast : MTLMathModeSafe;
        options.mathFloatingPointFunctions = fast ? MTLMathFloatingPointFunctionsFast
                                                  : MTLMathFloatingPointFunctionsPrecise;
    } else {
        options.fastMathEnabled = fast;
    }
    NSError *error = nil;
    id<MTLLibrary> result = [device newLibraryWithSource:source options:options error:&error];
    require(result != nil, error.localizedDescription ?: @"Library unavailable");
    return result;
}
static NSArray *render(id<MTLDevice> device, id<MTLCommandQueue> queue,
                      id<MTLFunction> vertex, id<MTLFunction> fragment,
                      const float input[4],
                      const float expected[4], BOOL *matches) {
    NSError *error = nil;
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vertex; pd.fragmentFunction = fragment;
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
    require(pipeline != nil, error.localizedDescription ?: @"Pipeline unavailable");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
        MTLPixelFormatRGBA32Float width:1 height:1 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    id<MTLBuffer> readback = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    require(texture != nil && readback != nil, @"Readback resources unavailable");
    // The original DXSO cbuffer is 224 float4 + 16 int4 entries. c0 is supplied
    // after library/pipeline compilation; no zero/normal value is in the MSL.
    id<MTLBuffer> uniforms = [device newBufferWithLength:240*16 options:MTLResourceStorageModeShared];
    id<MTLArgumentEncoder> arguments = [fragment newArgumentEncoderWithBufferIndex:0];
    require(uniforms != nil && arguments != nil, @"Runtime input bindings unavailable");
    memset(uniforms.contents, 0, uniforms.length); memcpy(uniforms.contents, input, 4*sizeof(float));
    id<MTLBuffer> bindings = [device newBufferWithLength:arguments.encodedLength options:MTLResourceStorageModeShared];
    require(bindings != nil, @"Argument buffer unavailable");
    [arguments setArgumentBuffer:bindings offset:0];
    [arguments setBuffer:uniforms offset:0 atIndex:0];
    memset(readback.contents, 0xa7, 256);
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(-123, -124, -125, -126);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    require(command != nil && encoder != nil, @"Render encoder unavailable");
    [encoder setRenderPipelineState:pipeline];
    [encoder setViewport:(MTLViewport){0, 0, 1, 1, 0, 1}];
    const float state[8] = {};
    [encoder setFragmentBuffer:bindings offset:0 atIndex:0];
    [encoder useResource:uniforms usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [encoder setFragmentBytes:state length:sizeof state atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    require(blit != nil, @"Blit encoder unavailable");
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
        sourceSize:MTLSizeMake(1,1,1) toBuffer:readback destinationOffset:0
        destinationBytesPerRow:256 destinationBytesPerImage:256];
    [blit endEncoding];
    [command commit]; [command waitUntilCompleted]; // external process deadline is mandatory
    require(command.status == MTLCommandBufferStatusCompleted,
            command.error.localizedDescription ?: @"GPU command failed");
    float values[4]; memcpy(values, readback.contents, sizeof values);
    NSMutableArray *lanes = [NSMutableArray new]; *matches = YES;
    for (unsigned i=0; i<4; ++i) {
        const bool finite = std::isfinite(values[i]);
        // FLT_MAX and zero boundaries must be exact; ordinary transcendental
        // inputs allow a small relative error, never a NaN/Inf substitute.
        const bool equal = finite && (expected[i] == 0 || fabsf(expected[i]) == FLT_MAX
            ? values[i] == expected[i]
            : fabsf(values[i]-expected[i]) <= fmaxf(1.f,fabsf(expected[i])) * 0.00001f);
        *matches &= equal;
        uint32_t bits; memcpy(&bits, &values[i], sizeof bits);
        [lanes addObject:@{@"bits":@(bits), @"finite":@(finite), @"matches":@(equal)}];
    }
    return lanes;
}
int main(int argc, char **argv) {
    @autoreleasepool {
      try {
        require(argc == 2, @"Authored MSL directory required");
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            puts("{\"schema\":1,\"status\":\"unavailable\",\"metal_execution_verified\":false}");
            return 77;
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        require(queue != nil, @"Command queue unavailable");
        NSString *vs = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "vertex float4 triangle(uint i [[vertex_id]]) { const float2 p[3]="
            "{float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0,1); }";
        id<MTLFunction> vertex = [library(device, vs, false) newFunctionWithName:@"triangle"];
        require(vertex != nil, @"Vertex unavailable");
        const char *operations[] = {"rcp", "rsq", "nrm", "log", "pow"};
        NSMutableArray *results = [NSMutableArray new]; BOOL all = YES;
        NSString *directory = [NSString stringWithUTF8String:argv[1]];
        for (unsigned op=0; op<5; ++op) for (unsigned fast=0; fast<2; ++fast) {
            @autoreleasepool {
            NSString *shader = [NSString stringWithFormat:@"float-%s-runtime", operations[op]];
            NSError *error = nil;
            NSString *source = [NSString stringWithContentsOfFile:
                [directory stringByAppendingPathComponent:[shader stringByAppendingString:@".metal"]]
                encoding:NSUTF8StringEncoding error:&error];
            require(source != nil && source.length < 65536,
                    error.localizedDescription ?: @"Authored MSL unavailable or oversized");
            id<MTLLibrary> lib = library(device, source, fast);
            MTLFunctionConstantValues *constants = [MTLFunctionConstantValues new];
            uint32_t always = 7;
            [constants setConstantValue:&always type:MTLDataTypeUInt atIndex:1225];
            id<MTLFunction> fragment = [lib newFunctionWithName:@"main0" constantValues:constants error:&error];
            require(fragment != nil, error.localizedDescription ?: @"Fragment unavailable");
            for (unsigned normal=0; normal<2; ++normal) {
                NSString *name = [NSString stringWithFormat:@"float-%s-%s", operations[op], normal ? "normal" : "zero"];
                const float input[4] = {normal ? (op==2 ? 3.f : op==0 || op==4 ? 2.f : 4.f) : 0.f,
                    normal ? (op==2 ? 4.f : op==4 ? 3.f : 0.f) : 0.f, 0.f, 0.f};
                float expected[4];
                float value = normal ? (op==0 || op==1 ? .5f : op==3 ? 2.f : 8.f)
                                     : (op==3 ? -FLT_MAX : op==4 ? 1.f : FLT_MAX);
                for (float &v : expected) v = value;
                if (op==2) { expected[0]=normal ? .6f : 0; expected[1]=normal ? .8f : 0; expected[2]=expected[3]=0; }
                BOOL matches;
                NSArray *lanes = render(device, queue, vertex, fragment, input, expected, &matches);
                all &= matches;
                [results addObject:@{@"case":name,@"math_mode":fast ? @"fast" : @"safe",
                    @"matches":@(matches),@"lanes":lanes}];
            }
            }
        }
        NSDictionary *report = @{@"schema":@1,@"status":all ? @"passed" : @"failed",
            @"case_count":@(results.count),@"public_only":@YES,@"runtime_inputs":@YES,@"metal_execution_verified":@YES,
            @"physical_iphone_verified":@NO,@"game_rendering_verified":@NO,@"cases":results};
        NSData *json = [NSJSONSerialization dataWithJSONObject:report options:NSJSONWritingPrettyPrinted error:nil];
        require(json != nil, @"Report encoding failed");
        fwrite(json.bytes,1,json.length,stdout);putchar('\n');
        return all ? 0 : 1;
      } catch (const std::exception &error) {
        fprintf(stderr,"Authored Metal arithmetic verification failed: %s\n",error.what());
        return 2;
      }
    }
}
