// mtldraw - Metal *drawing* test on the Iris Xe (mtlprobe only tested compute). Renders into an off-screen
// 64x64 texture, copies it back with a blit and checks the pixels. Nothing touches the desktop.
// Each test also writes a PNG next to the program (mtldraw-1-clear.png ...) so you can look at the result.
// The plugin always gives a process named "mtldraw" a device, whatever the allow list says.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#define W 64
#define H 64

static int fails;
static void step(const char *what, BOOL ok, NSString *detail)
{
    printf("[%s] %-38s %s\n", ok ? " OK " : "FAIL", what, detail ? detail.UTF8String : "");
    if (!ok) fails++;
}

typedef struct { float x, y, r, g, b, a, u, v; } Vtx;

static id<MTLDevice> dev;
static id<MTLCommandQueue> q;
static id<MTLBuffer> readback;
static BOOL gExplicitVP;          // set the viewport, scissor and cull mode by hand
static id<MTLBuffer> gDbg;          // vertex-stage debug output (buffer 1)

static void save_png(const char *name)
{
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(readback.contents, W, H, 8, W * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    NSString *path = [[NSFileManager defaultManager].currentDirectoryPath stringByAppendingPathComponent:@(name)];
    CGImageDestinationRef d = img ? CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                                    CFSTR("public.png"), 1, NULL) : NULL;
    if (d) { CGImageDestinationAddImage(d, img, NULL); CGImageDestinationFinalize(d); CFRelease(d); }
    if (img) CGImageRelease(img);
    if (ctx) CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
}

// pixel (x, y) from the top-left, as r g b a 0..255
static void px(int x, int y, int out[4])
{
    const uint8_t *p = (const uint8_t *)readback.contents + (y * W + x) * 4;   // BGRA
    out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3];
}
static BOOL near(const int c[4], int r, int g, int b)
{
    return abs(c[0] - r) <= 12 && abs(c[1] - g) <= 12 && abs(c[2] - b) <= 12;
}
static NSString *desc(const int c[4]) { return [NSString stringWithFormat:@"(%d,%d,%d)", c[0], c[1], c[2]]; }

// one render pass: clear, optionally draw, then blit the texture into `readback`; returns the command buffer status text
static NSString *pass(id<MTLTexture> target, MTLClearColor clear, id<MTLRenderPipelineState> pso,
                      id<MTLBuffer> vb, NSUInteger nverts, MTLPrimitiveType prim, id<MTLTexture> tex, id<MTLSamplerState> smp,
                      BOOL *ok)
{
    memset(readback.contents, 0x55, W * H * 4);
    id<MTLCommandBuffer> cb = [q commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = clear;
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
    if (pso) {
        [re setRenderPipelineState:pso];
        if (gExplicitVP) {
            [re setViewport:(MTLViewport){0, 0, W, H, 0, 1}];
            [re setScissorRect:(MTLScissorRect){0, 0, W, H}];
            [re setCullMode:MTLCullModeNone];
            [re setFrontFacingWinding:MTLWindingCounterClockwise];
        }
        if (vb) [re setVertexBuffer:vb offset:0 atIndex:0];
        if (gDbg) [re setVertexBuffer:gDbg offset:0 atIndex:1];
        if (tex) [re setFragmentTexture:tex atIndex:0];
        if (smp) [re setFragmentSamplerState:smp atIndex:0];
        [re drawPrimitives:prim vertexStart:0 vertexCount:nverts];
    }
    [re endEncoding];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:readback destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    *ok = cb.status == MTLCommandBufferStatusCompleted;
    return [NSString stringWithFormat:@"status %ld%@", (long)cb.status,
            cb.error ? [@" " stringByAppendingString:cb.error.localizedDescription] : @""];
}

int main(void)
{
    @autoreleasepool {
        dev = MTLCreateSystemDefaultDevice();
        step("Metal device", dev != nil, dev ? dev.name : @"nil");
        if (!dev) { printf("\nRESULT: no Metal device\n"); return 1; }
        q = [dev newCommandQueue];
        readback = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];

        NSError *err = nil;
        NSString *src = @
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { packed_float2 pos; packed_float4 col; packed_float2 uv; };   /* 32 bytes, like the C struct Vtx */\n"
            "struct O { float4 pos [[position]]; float4 col; float2 uv; };\n"
            "vertex O vmain(uint id [[vertex_id]], const device V *v [[buffer(0)]]) {\n"
            "  O o; o.pos = float4(v[id].pos, 0, 1); o.col = v[id].col; o.uv = v[id].uv; return o; }\n"
            "vertex O vconst(uint id [[vertex_id]]) {\n"
            "  const float2 p[3] = { float2(-0.8, -0.8), float2(0.8, -0.8), float2(0.0, 0.8) };\n"
            "  O o; o.pos = float4(p[id], 0, 1); o.col = float4(1, 0, 0, 1); o.uv = float2(0); return o; }\n"
            "vertex O vdbg(uint id [[vertex_id]], const device V *v [[buffer(0)]], device float4 *dbg [[buffer(1)]]) {\n"
            "  dbg[id] = float4(v[id].pos, v[id].col.r, 42.0);\n"
            "  O o; o.pos = float4(v[id].pos, 0, 1); o.col = v[id].col; o.uv = v[id].uv; return o; }\n"
            "fragment float4 fsolid(O i [[stage_in]]) { return float4(1, 0, 0, 1); }\n"
            "fragment float4 fcolor(O i [[stage_in]]) { return i.col; }\n"
            "fragment float4 ftex(O i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
            "  return t.sample(s, i.uv); }\n";
        id<MTLLibrary> lib = [dev newLibraryWithSource:src options:nil error:&err];
        step("compile vertex + fragment shaders", lib != nil, err ? err.localizedDescription : nil);
        if (!lib) { printf("\nRESULT: see the FAIL lines\n"); return 2; }

        id<MTLRenderPipelineState> psoSolid = nil, psoColor = nil, psoTex = nil;
        NSString *names[3] = { @"fsolid", @"fcolor", @"ftex" };
        id<MTLRenderPipelineState> __strong *slots[3] = { &psoSolid, &psoColor, &psoTex };
        for (int k = 0; k < 3; k++) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
            pd.fragmentFunction = [lib newFunctionWithName:names[k]];
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            err = nil;
            *slots[k] = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
            step([NSString stringWithFormat:@"render pipeline (%@)", names[k]].UTF8String, *slots[k] != nil,
                 err ? err.localizedDescription : nil);
        }

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> target = [dev newTextureWithDescriptor:td];
        step("64x64 render target texture", target != nil, nil);
        if (!target) { printf("\nRESULT: see the FAIL lines\n"); return 3; }

        BOOL ok; NSString *st; int c[4], d[4];

        // ---- diagnostics: why draws come out empty (10-17: clears worked, every draw left the image untouched) ----
        id<MTLRenderPipelineState> psoConst = nil, psoDbg = nil;
        {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [lib newFunctionWithName:@"vconst"];
            pd.fragmentFunction = [lib newFunctionWithName:@"fsolid"];
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
            err = nil; psoConst = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
            step("D pipeline: constant triangle (no buffers)", psoConst != nil, err ? err.localizedDescription : nil);
            pd.vertexFunction = [lib newFunctionWithName:@"vdbg"];
            err = nil; psoDbg = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
            step("D pipeline: vertex debug writer", psoDbg != nil, err ? err.localizedDescription : nil);
        }
        Vtx dtri[3] = { {-0.8f, -0.8f, 1,0,0,1, 0,0}, {0.8f, -0.8f, 0,1,0,1, 0,0}, {0.0f, 0.8f, 0,0,1,1, 0,0} };
        id<MTLBuffer> dvb = [dev newBufferWithBytes:dtri length:sizeof dtri options:MTLResourceStorageModeShared];
        for (int vp = 0; vp < 2 && psoConst; vp++) {
            gExplicitVP = vp;
            st = pass(target, MTLClearColorMake(0, 0, 0, 1), psoConst, nil, 3, MTLPrimitiveTypeTriangle, nil, nil, &ok);
            px(32, 40, c);
            step(vp ? "D constant triangle, explicit viewport" : "D constant triangle, default viewport", ok && near(c, 255, 0, 0),
                 [NSString stringWithFormat:@"%@, pixel(32,40) = %@", st, desc(c)]);
            if (vp) save_png("mtldraw-D-const.png");
        }
        if (psoDbg) {
            gDbg = [dev newBufferWithLength:3 * 16 options:MTLResourceStorageModeShared];
            memset(gDbg.contents, 0, 3 * 16);
            gExplicitVP = YES;
            st = pass(target, MTLClearColorMake(0, 0, 0, 1), psoDbg, dvb, 3, MTLPrimitiveTypeTriangle, nil, nil, &ok);
            const float *f = gDbg.contents;
            BOOL ran = f[3] == 42.0f || f[7] == 42.0f || f[11] == 42.0f;
            BOOL read = ran && f[0] < -0.7f && f[4] > 0.7f && f[9] > 0.7f;
            step("D vertex shader ran (wrote its marker)", ok && ran, [NSString stringWithFormat:@"%@, markers %.0f %.0f %.0f", st, f[3], f[7], f[11]]);
            step("D vertex shader read the vertex buffer", read,
                 [NSString stringWithFormat:@"v0 (%.2f,%.2f) v1 (%.2f,%.2f) v2 (%.2f,%.2f), want (-0.8,-0.8) (0.8,-0.8) (0,0.8)",
                  f[0], f[1], f[4], f[5], f[8], f[9]]);
            px(32, 40, c);
            step("D buffer triangle, explicit viewport", near(c, 255, 0, 0), [NSString stringWithFormat:@"pixel(32,40) = %@", desc(c)]);
            gDbg = nil;
        }
        gExplicitVP = NO;
        printf("       (tests 1-4 below use Metal's default viewport, as most apps do)\n");

        // 1: clear only (render pass load/store + blit readback)
        st = pass(target, MTLClearColorMake(0, 0, 1, 1), nil, nil, 0, 0, nil, nil, &ok);
        step("1 clear to blue: GPU run", ok, st);
        int bad = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { px(x, y, c); if (!near(c, 0, 0, 255)) bad++; }
        px(10, 10, c);
        step("1 clear to blue: pixels", bad == 0, [NSString stringWithFormat:@"%d of %d wrong, pixel(10,10) = %@", bad, W * H, desc(c)]);
        save_png("mtldraw-1-clear.png");

        // 2: one solid red triangle on black
        Vtx tri[3] = { {-0.8f, -0.8f, 1,0,0,1, 0,0}, {0.8f, -0.8f, 0,1,0,1, 0,0}, {0.0f, 0.8f, 0,0,1,1, 0,0} };
        id<MTLBuffer> vb = [dev newBufferWithBytes:tri length:sizeof tri options:MTLResourceStorageModeShared];
        if (psoSolid) {
            st = pass(target, MTLClearColorMake(0, 0, 0, 1), psoSolid, vb, 3, MTLPrimitiveTypeTriangle, nil, nil, &ok);
            step("2 red triangle: GPU run", ok, st);
            px(32, 40, c); px(2, 2, d);
            step("2 red triangle: inside is red", near(c, 255, 0, 0), [NSString stringWithFormat:@"pixel(32,40) = %@", desc(c)]);
            step("2 red triangle: corner stays black", near(d, 0, 0, 0), [NSString stringWithFormat:@"pixel(2,2) = %@", desc(d)]);
            save_png("mtldraw-2-triangle.png");
        }

        // 3: the same triangle with per-vertex colours (interpolation)
        if (psoColor) {
            st = pass(target, MTLClearColorMake(0, 0, 0, 1), psoColor, vb, 3, MTLPrimitiveTypeTriangle, nil, nil, &ok);
            step("3 colour triangle: GPU run", ok, st);
            px(32, 40, c);
            BOOL mixed = c[0] > 30 && c[1] > 30 && c[2] > 30;
            step("3 colour triangle: centre is a blend", mixed, [NSString stringWithFormat:@"pixel(32,40) = %@", desc(c)]);
            px(10, 55, c); px(54, 55, d);
            step("3 colour triangle: corners red / green", c[0] > c[1] && c[0] > c[2] && d[1] > d[0] && d[1] > d[2],
                 [NSString stringWithFormat:@"bottom-left %@, bottom-right %@", desc(c), desc(d)]);
            save_png("mtldraw-3-colours.png");
        }

        // 4: a full-screen quad sampling a 2x2 texture (red, green / blue, white), nearest filtering
        if (psoTex) {
            MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                          width:2 height:2 mipmapped:NO];
            sd.usage = MTLTextureUsageShaderRead;
            sd.storageMode = MTLStorageModeManaged;
            id<MTLTexture> small = [dev newTextureWithDescriptor:sd];
            const uint8_t texels[16] = { 0,0,255,255,  0,255,0,255,    // row 0: red, green   (BGRA)
                                         255,0,0,255,  255,255,255,255 }; // row 1: blue, white
            [small replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:texels bytesPerRow:8];
            MTLSamplerDescriptor *smd = [MTLSamplerDescriptor new];
            smd.minFilter = smd.magFilter = MTLSamplerMinMagFilterNearest;
            id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:smd];
            Vtx quad[4] = { {-1, -1, 1,1,1,1, 0,1}, {1, -1, 1,1,1,1, 1,1}, {-1, 1, 1,1,1,1, 0,0}, {1, 1, 1,1,1,1, 1,0} };
            id<MTLBuffer> qb = [dev newBufferWithBytes:quad length:sizeof quad options:MTLResourceStorageModeShared];
            step("4 texture + sampler created", small != nil && smp != nil, nil);
            st = pass(target, MTLClearColorMake(0, 0, 0, 1), psoTex, qb, 4, MTLPrimitiveTypeTriangleStrip, small, smp, &ok);
            step("4 textured quad: GPU run", ok, st);
            int tl[4], tr[4], bl[4], br[4];
            px(16, 16, tl); px(48, 16, tr); px(16, 48, bl); px(48, 48, br);
            step("4 textured quad: four quadrants", near(tl, 255, 0, 0) && near(tr, 0, 255, 0) && near(bl, 0, 0, 255) && near(br, 255, 255, 255),
                 [NSString stringWithFormat:@"TL %@ TR %@ BL %@ BR %@ (want red green blue white)", desc(tl), desc(tr), desc(bl), desc(br)]);
            save_png("mtldraw-4-texture.png");
        }

        printf("\nPNG files written to %s\n", [NSFileManager defaultManager].currentDirectoryPath.UTF8String);
        printf("RESULT: %s\n", fails ? "see the FAIL lines" : "Metal drawing works on the Iris Xe");
        return fails ? 10 : 0;
    }
}
