// mtlprobe - first Metal test on the Iris Xe (NMXAccel + NVMTLDriver plugin + Mesa ANV). The plugin always gives
// a process named "mtlprobe" a device, whatever the allow list says. Each step prints OK/FAIL.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static int fails;
static void step(const char *what, BOOL ok, NSString *detail)
{
    printf("[%s] %-34s %s\n", ok ? " OK " : "FAIL", what, detail ? detail.UTF8String : "");
    if (!ok) fails++;
}

int main(void)
{
    @autoreleasepool {
        NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
        step("MTLCopyAllDevices", all.count > 0, [NSString stringWithFormat:@"%lu device(s)", (unsigned long)all.count]);
        for (id<MTLDevice> d in all)
            printf("       %s  registryID 0x%llx  lowPower %d  headless %d\n", d.name.UTF8String, d.registryID, d.lowPower, d.headless);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        step("MTLCreateSystemDefaultDevice", dev != nil, dev ? dev.name : @"nil");
        if (!dev) { printf("\nRESULT: no Metal device\n"); return 1; }

        id<MTLBuffer> buf = [dev newBufferWithLength:1024 * 4 options:MTLResourceStorageModeShared];
        step("newBufferWithLength (4 KiB)", buf != nil, nil);
        if (!buf) return 2;
        memset(buf.contents, 0xAB, 1024 * 4);

        NSError *err = nil;
        NSString *src = @"#include <metal_stdlib>\nusing namespace metal;\n"
                        "kernel void fill(device uint *v [[buffer(0)]], uint i [[thread_position_in_grid]]) { v[i] = i * 3 + 7; }\n";
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        step("compile Metal shader", lib != nil, err ? err.localizedDescription : nil);
        if (!lib) return 3;
        id<MTLFunction> fn = [lib newFunctionWithName:@"fill"];
        step("find kernel 'fill'", fn != nil, nil);
        if (!fn) return 4;
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
        step("compute pipeline", pso != nil, err ? err.localizedDescription : nil);
        if (!pso) return 5;

        id<MTLCommandQueue> q = [dev newCommandQueue];
        step("command queue", q != nil, nil);
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        [enc setBuffer:buf offset:0 atIndex:0];
        [enc dispatchThreads:MTLSizeMake(1024, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        step("run on GPU", cb.status == MTLCommandBufferStatusCompleted,
             [NSString stringWithFormat:@"status %ld%@", (long)cb.status, cb.error ? [@" " stringByAppendingString:cb.error.localizedDescription] : @""]);

        const uint32_t *v = buf.contents;
        int bad = 0;
        for (uint32_t i = 0; i < 1024; i++)
            if (v[i] != i * 3 + 7) { if (bad < 5) printf("       v[%u] = 0x%08x, expected %u\n", i, v[i], i * 3 + 7); bad++; }
        step("check 1024 values", bad == 0, [NSString stringWithFormat:@"%d wrong", bad]);
        printf("\nRESULT: %s\n", fails ? "see the FAIL lines" : "Metal compute works on the Iris Xe");
    }
    return fails ? 2 : 0;
}
