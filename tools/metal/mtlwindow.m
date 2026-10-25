// mtlwindow - first on-screen Metal test on the Iris Xe: a window with a CAMetalLayer and a spinning colour triangle.
// mtldraw proved off-screen drawing; this proves the path from a finished GPU frame to the window (drawable ->
// presentDrawable -> WindowServer). Every 2 s it prints frames per second, CPU use and a GPU-side pixel check of the
// last frame (read back from the drawable), so "the GPU drew it" and "it reached the screen" can be told apart.
// Runs for 20 s (or `./mtlwindow 60` for 60 s) and then prints a summary. The plugin always gives "mtlwindow" a device.
#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <unistd.h>

// modes (second argument): "offscreen" draws into its own texture (no drawable, nothing shown), "once" presents a
// single frame and then only waits, "" = the full 60 fps window. Each step is also written to ~/mtlwindow-steps.txt
// with fsync, so it survives a hard freeze.
static int gMode;   // 0 full, 1 offscreen, 2 once
static int gStepFd = -1;
static void stepmark(const char *fmt, ...)
{
    char b[256]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    if (gStepFd < 0) {
        char p[512]; snprintf(p, sizeof p, "%s/mtlwindow-steps.txt", getenv("HOME") ?: "/tmp");
        gStepFd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (gStepFd >= 0) { char l[300]; int n = snprintf(l, sizeof l, "%.3f %s\n", CACurrentMediaTime(), b); write(gStepFd, l, n); fsync(gStepFd); }
}

static double now_s(void) { return CACurrentMediaTime(); }
static double cpu_s(void)
{
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

@interface MTLWView : NSView
@end
@implementation MTLWView
- (BOOL)wantsUpdateLayer { return YES; }
- (CALayer *)makeBackingLayer { return [CAMetalLayer layer]; }
@end

@interface Runner : NSObject <NSApplicationDelegate>
@end

@implementation Runner {
    NSWindow *_win; CAMetalLayer *_layer;
    id<MTLDevice> _dev; id<MTLCommandQueue> _q; id<MTLRenderPipelineState> _pso; id<MTLBuffer> _probe; id<MTLTexture> _off;
    double _t0, _lastReport, _cpu0, _cpuLast, _duration;
    unsigned long _frames, _framesLast, _noDrawable, _cbErrors, _pixOK, _pixChecks;
    NSString *_lastErr;
}

- (instancetype)initWithDuration:(double)d { if ((self = [super init])) _duration = d; return self; }

- (void)applicationDidFinishLaunching:(NSNotification *)n
{
    stepmark("launch, mode %d", gMode);
    _dev = MTLCreateSystemDefaultDevice();
    stepmark("device %s", _dev ? "ok" : "nil");
    printf("[%s] Metal device                %s\n", _dev ? " OK " : "FAIL", _dev ? _dev.name.UTF8String : "nil");
    if (!_dev) { [NSApp terminate:nil]; return; }

    NSRect r = NSMakeRect(200, 200, 640, 480);
    _win = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                         backing:NSBackingStoreBuffered defer:NO];
    _win.title = @"mtlwindow - Metal on the Iris Xe";
    MTLWView *v = [[MTLWView alloc] initWithFrame:r];
    v.wantsLayer = YES;
    _win.contentView = v;
    _layer = (CAMetalLayer *)v.layer;
    _layer.device = _dev;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _layer.framebufferOnly = NO;                       // so the frame can be read back for the pixel check
    _layer.contentsScale = _win.backingScaleFactor;
    _layer.drawableSize = CGSizeMake(640 * _win.backingScaleFactor, 480 * _win.backingScaleFactor);
    printf("[ OK ] CAMetalLayer                %s, drawable %.0fx%.0f\n", [_layer isKindOfClass:[CAMetalLayer class]] ? "yes" : "NO",
           _layer.drawableSize.width, _layer.drawableSize.height);
    [_win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    NSError *err = nil;
    NSString *src = @
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct O { float4 pos [[position]]; float4 col; };\n"
        "vertex O vmain(uint id [[vertex_id]], constant float &t [[buffer(0)]]) {\n"
        "  float a = t + id * 2.0943951;\n"
        "  O o; o.pos = float4(0.7 * cos(a), 0.7 * sin(a), 0, 1);\n"
        "  o.col = float4(id == 0, id == 1, id == 2, 1); return o; }\n"
        "fragment float4 fmain(O i [[stage_in]]) { return i.col; }\n";
    id<MTLLibrary> lib = [_dev newLibraryWithSource:src options:nil error:&err];
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    pd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    _pso = lib ? [_dev newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
    printf("[%s] shaders + pipeline          %s\n", _pso ? " OK " : "FAIL", err ? err.localizedDescription.UTF8String : "");
    if (!_pso) { [NSApp terminate:nil]; return; }
    stepmark("pipeline %s", _pso ? "ok" : "FAILED");
    _q = [_dev newCommandQueue];
    if (gMode == 1) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:640 height:480 mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        _off = [_dev newTextureWithDescriptor:td];
        printf("[%s] off-screen texture          (mode offscreen: nothing is drawn into the window)\n", _off ? " OK " : "FAIL");
    }
    _probe = [_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];

    _t0 = _lastReport = now_s(); _cpu0 = _cpuLast = cpu_s();
    printf("\nrunning %.0f s - the window should show a spinning red/green/blue triangle on dark blue\n", _duration);
    printf("  time   fps   cpu%%   frames  no-drawable  gpu-errors  centre-pixel\n");
    [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 target:self selector:@selector(frame) userInfo:nil repeats:YES];
}

- (void)frame
{
    double t = now_s() - _t0;
    if (t > _duration) { [self finish]; return; }
    @autoreleasepool {
        if (gMode == 2 && _frames >= 1) { [self report:t]; return; }   // "once": one frame, then just watch
        const BOOL early = _frames < 5;
        id<CAMetalDrawable> d = nil;
        id<MTLTexture> target = _off;
        if (!_off) {
            if (early) stepmark("frame %lu: nextDrawable", _frames);
            d = [_layer nextDrawable];
            if (early) stepmark("frame %lu: drawable %s", _frames, d ? "ok" : "nil");
            if (!d) { _noDrawable++; [self report:t]; return; }
            target = d.texture;
        }
        id<MTLCommandBuffer> cb = [_q commandBuffer];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = target;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0.05, 0.08, 0.25, 1);
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
        float ft = (float)t * 2.0f;
        [re setRenderPipelineState:_pso];
        [re setVertexBytes:&ft length:sizeof ft atIndex:0];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
        BOOL check = (_frames % 60) == 30;
        if (check) {      // copy the centre pixel of this frame back: proves the GPU drew into the drawable itself
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            NSUInteger cx = target.width / 2, cy = target.height / 2;
            [bl copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(cx, cy, 0)
                     sourceSize:MTLSizeMake(1, 1, 1) toBuffer:_probe destinationOffset:0 destinationBytesPerRow:4
                destinationBytesPerImage:4];
            [bl endEncoding];
        }
        if (d) [cb presentDrawable:d];
        __unsafe_unretained Runner *me = self;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            if (c.status == MTLCommandBufferStatusError) {
                dispatch_async(dispatch_get_main_queue(), ^{ me->_cbErrors++; me->_lastErr = c.error.localizedDescription; });
            } else if (check) {
                const uint8_t *p = me->_probe.contents;     // BGRA; the centre is inside the triangle every frame
                BOOL coloured = (p[0] + p[1] + p[2]) > 150 && !(p[0] > 50 && p[1] < 30 && p[2] < 20);
                dispatch_async(dispatch_get_main_queue(), ^{ me->_pixChecks++; if (coloured) me->_pixOK++; });
            }
        }];
        if (early) stepmark("frame %lu: commit%s", _frames, d ? " + present" : "");
        [cb commit];
        if (early) { [cb waitUntilCompleted]; stepmark("frame %lu: completed, status %ld", _frames, (long)cb.status); }
        _frames++;
    }
    [self report:t];
}

- (void)report:(double)t
{
    double n = now_s();
    if (n - _lastReport < 2.0) return;
    double c = cpu_s();
    const uint8_t *p = _probe.contents;
    printf("  %4.0fs %5.1f %5.1f %8lu %12lu %11lu   rgb(%d,%d,%d) %lu/%lu ok\n", t, (_frames - _framesLast) / (n - _lastReport),
           100.0 * (c - _cpuLast) / (n - _lastReport), _frames, _noDrawable, _cbErrors, p[2], p[1], p[0], _pixOK, _pixChecks);
    fflush(stdout);
    _lastReport = n; _framesLast = _frames; _cpuLast = c;
}

- (void)finish
{
    static BOOL done; if (done) return; done = YES;
    double el = now_s() - _t0;
    printf("\nSUMMARY: %lu frames in %.1f s = %.1f fps, CPU %.0f%% of one core, %lu missing drawables, %lu GPU errors, "
           "pixel check %lu/%lu\n", _frames, el, _frames / el, 100.0 * (cpu_s() - _cpu0) / el, _noDrawable, _cbErrors, _pixOK, _pixChecks);
    if (_lastErr) printf("last GPU error: %s\n", _lastErr.UTF8String);
    BOOL gpuOK = _frames > 30 && _cbErrors == 0 && _pixChecks && _pixOK == _pixChecks;
    printf("GPU side: %s\n", gpuOK ? "frames rendered correctly into the window's drawables" : "PROBLEM - see the numbers above");
    stepmark("finished cleanly");
    printf("Now tell me what the WINDOW showed: spinning triangle / frozen triangle / black / dark blue only / garbage\n");
    [NSApp terminate:nil];
}
@end

int main(int argc, char **argv)
{
    @autoreleasepool {
        double dur = argc > 1 ? atof(argv[1]) : 20;
        if (argc > 2) gMode = !strcmp(argv[2], "offscreen") ? 1 : !strcmp(argv[2], "once") ? 2 : 0;
        if (dur < 3) dur = 20;
        NSApplication *app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        Runner *r = [[Runner alloc] initWithDuration:dur];
        app.delegate = r;
        [app run];
    }
    return 0;
}
