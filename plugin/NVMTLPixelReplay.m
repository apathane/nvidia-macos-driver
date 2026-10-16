/* Optional, bounded snapshots of published CPU-backed BGRA/RGBA IOSurfaces.
 * No GPU readbacks and no capture of uncertain, tiled or unsupported formats. */
static void nvmtl_pixel_replay_capture(NSSet *surfaces)
{
    static dispatch_once_t once;
    static dispatch_queue_t writer;
    static NSString *directory;
    static unsigned frames;
    static size_t total;
    dispatch_once(&once, ^{
        const char *e = getenv("NVMTL_PIXEL_REPLAY");
        BOOL enabled = e && e[0] == '1';
        if (!e && !strcmp(getprogname(), "WindowServer"))
            enabled = [[NSFileManager defaultManager] fileExistsAtPath:@"/Library/Preferences/local.nullmoth.pixel-replay.enable"];
        if (!enabled) { nvlog("pixel replay: disabled (environment or activation marker)"); return; }
        directory = [NSTemporaryDirectory() stringByAppendingPathComponent:
            [NSString stringWithFormat:@"nvmtl-pixels-%d-%llu", getpid(),
                (unsigned long long)clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)]];
        if (![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
            attributes:@{NSFilePosixPermissions:@0700} error:NULL]) { nvlog("pixel replay: could not create private capture directory"); directory = nil; return; }
        writer = dispatch_queue_create("nvmtl.pixel-replay", DISPATCH_QUEUE_SERIAL);
        nvlog("pixel replay: bounded capture at %s", directory.fileSystemRepresentation);
    });
    if (!writer) return;
    @synchronized (directory) {
        for (NVMTLTexture *view in surfaces) {
            if (frames >= 32 || total >= (128u << 20)) break;
            NVMTLTexture *t = view;
            while (t->_parent || t->_viewParent) t = t->_parent ?: t->_viewParent;
            if (!t->_surf || t->_vramOn || t->_plane || IOSurfaceGetPlaneCount(t->_surf)) continue;
            BOOL bgra = t->_fmt == MTLPixelFormatBGRA8Unorm || t->_fmt == MTLPixelFormatBGRA8Unorm_sRGB;
            BOOL rgba = t->_fmt == MTLPixelFormatRGBA8Unorm || t->_fmt == MTLPixelFormatRGBA8Unorm_sRGB;
            if (!bgra && !rgba) continue;
            @synchronized (nvmtl_surface_sync(t)) {
                size_t w = IOSurfaceGetWidth(t->_surf), h = IOSurfaceGetHeight(t->_surf);
                size_t row = IOSurfaceGetBytesPerRow(t->_surf), alloc = IOSurfaceGetAllocSize(t->_surf);
                if (w < 64 || h < 64 || w > 8192 || h > 8192 || row < w * 4 ||
                    row > alloc / h || w * h * 4 > (128u << 20) - total) continue;
                if (IOSurfaceLock(t->_surf, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) continue;
                const char *base = IOSurfaceGetBaseAddress(t->_surf);
                NSMutableData *pixels = base ? [NSMutableData dataWithLength:w * h * 4] : nil;
                if (pixels) for (size_t y = 0; y < h; y++)
                    memcpy((char *)pixels.mutableBytes + y * w * 4, base + y * row, w * 4);
                uint32_t seed = IOSurfaceGetSeed(t->_surf);
                IOReturn unlocked = IOSurfaceUnlock(t->_surf, kIOSurfaceLockReadOnly, NULL);
                if (!pixels || unlocked != kIOReturnSuccess) continue;
                unsigned seq = ++frames; total += pixels.length;
                NSString *name = [NSString stringWithFormat:@"%04u", seq];
                NSDictionary *meta = @{ @"sequence":@(seq), @"width":@(w), @"height":@(h),
                    @"surface":@(IOSurfaceGetID(t->_surf)), @"seed":@(seed),
                    @"format":@(t->_fmt), @"order":bgra ? @"BGRA" : @"RGBA",
                    @"time_ns":@(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)),
                    @"file":[name stringByAppendingString:@".pixels"] };
                dispatch_async(writer, ^{
                    @autoreleasepool {
                        NSString *path = [directory stringByAppendingPathComponent:meta[@"file"]];
                        NSData *json = [NSJSONSerialization dataWithJSONObject:meta options:0 error:NULL];
                        BOOL ok = [pixels writeToFile:path options:NSDataWritingAtomic error:NULL] &&
                            [json writeToFile:[directory stringByAppendingPathComponent:[name stringByAppendingString:@".json"]]
                                options:NSDataWritingAtomic error:NULL];
                        if (!ok) nvlog("pixel replay: failed to write frame %u", seq);
                    }
                });
            }
        }
    }
}
