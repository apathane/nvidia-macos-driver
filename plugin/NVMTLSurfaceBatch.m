#import <objc/objc-sync.h>

// Publish several private IOSurface images with one GPU submission and one wait.
// CPU readers and completion events cannot see the new seeds before the fence.
static BOOL nvmtl_publish_surfaces(NSSet *surfaces)
{
    const char *off = getenv("NVMTL_NO_SURFACE_BATCH");
    if (surfaces.count < 2 || surfaces.count > 16 || (off && off[0] == '1')) {
        BOOL ok = YES;
        for (NVMTLTexture *t in surfaces) if (![t nvmtlSurfaceOut]) ok = NO;
        return ok;
    }
    NSMutableArray *roots = [NSMutableArray new], *monitors = [NSMutableArray new];
    NSMutableSet *ids = [NSMutableSet new];
    BOOL eligible = YES;
    size_t bytes = 0;
    // Measured wins are in submission-bound small surfaces. Large readbacks are
    // bandwidth-bound and the extra multi-surface locking can cost more than it saves.
    const size_t maxBytes = 8u << 20;
    for (NVMTLTexture *t in surfaces) {
        NVMTLTexture *r = t->_parent ? t->_parent : t;
        // Shared VRAM already publishes without copying. Aliased planes and mixed
        // paths keep their original ordering and publication implementation.
        if (!r->_surf || r->_vramOn || [ids containsObject:@(IOSurfaceGetID(r->_surf))]) { eligible = NO; break; }
        size_t size = IOSurfaceGetAllocSize(r->_surf);
        if (size > maxBytes - bytes) { eligible = NO; break; }
        bytes += size;
        [ids addObject:@(IOSurfaceGetID(r->_surf))]; [roots addObject:r];
        [monitors addObject:nvmtl_surface_sync(r)];
    }
    if (!eligible) {
        BOOL ok = YES;
        for (NVMTLTexture *t in surfaces) if (![t nvmtlSurfaceOut]) ok = NO;
        return ok;
    }
    // Global order avoids lock inversions between concurrent multi-surface batches.
    NSArray *ordered = [monitors sortedArrayUsingComparator:^NSComparisonResult(id a, id b) {
        uintptr_t x = (uintptr_t)(__bridge void *)a, y = (uintptr_t)(__bridge void *)b;
        return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame;
    }];
    for (id monitor in ordered) objc_sync_enter(monitor);
    nvk_cmdbuf *cmd = calloc(1, sizeof *cmd);
    nvmtl_inflight *flight = nvmtl_vk_inflight_new();
    unsigned locked = 0;
    uint32_t layouts[16] = {0};
    const uint32_t lockopt = nvmtl_noseed() ? kIOSurfaceLockReadOnly : 0;
    BOOL prepared = cmd && flight && nvmtl_vk_cmd_begin(NULL, cmd) == 0;
    for (unsigned i = 0; prepared && i < roots.count; i++) {
        NVMTLTexture *r = roots[i];
        layouts[i] = r->_i.layout;
        if (![r nvmtlSurfaceCopyable] || IOSurfaceLock(r->_surf, lockopt, NULL) != kIOReturnSuccess) { prepared = NO; break; }
        locked++;
        void *base = IOSurfaceGetBaseAddressOfPlane(r->_surf, (uint32_t)r->_plane);
        size_t row = IOSurfaceGetBytesPerRowOfPlane(r->_surf, (uint32_t)r->_plane), offset = 0;
        nvk_buffer *buffer = (base && row) ? nvmtl_surfdma_buffer(r, base, row, &offset) : NULL;
        if (!buffer || nvmtl_vk_image_read_buffer_encode(cmd, &r->_i, buffer, offset, row)) prepared = NO;
    }
    int result = -1;
    if (prepared) {
        result = nvmtl_vk_submit_begin_at(cmd, flight, "surface readback batch");
        if (!result) result = nvmtl_vk_submit_finish(flight);
    } else if (cmd && cmd->cb) nvmtl_vk_cmd_abandon(cmd);
    const BOOL completed = result == 0;
    if (result == -2) {
        // The GPU may still be writing. Keep the surfaces, imports, wiring and
        // heap-allocated command state alive; never publish an uncertain result.
        static NSMutableArray *quarantine;
        @synchronized ([NVMTLTexture class]) {
            if (!quarantine) quarantine = [NSMutableArray new];
            [quarantine addObject:roots];
        }
        for (NVMTLTexture *r in roots) r->_coherence->surfaceSynced = NO;
        nvlog("surface batch: uncertain completion; retaining %u locked surfaces", locked);
    } else {
        for (unsigned i = 0; i < locked; i++) {
            NVMTLTexture *r = roots[i]; uint32_t seed = 0;
            IOReturn unlocked = IOSurfaceUnlock(r->_surf, lockopt, &seed);
            if (lockopt) seed = IOSurfaceGetSeed(r->_surf);
            if (!result && unlocked == kIOReturnSuccess) {
                r->_coherence->surfaceSeed = seed; r->_coherence->surfaceSynced = YES;
                r->_coherence->surfaceGeneration = nvmtl_vram_gen(r->_surf, r->_plane);
                __atomic_add_fetch(&g_surfdma_out, 1, __ATOMIC_RELAXED);
            } else {
                r->_coherence->surfaceSynced = NO;
                if (!result) result = -1;
            }
            if (!completed) r->_i.layout = layouts[i];
        }
        free(cmd); nvmtl_vk_inflight_free(flight);
    }
    for (id monitor in ordered.reverseObjectEnumerator) objc_sync_exit(monitor);
    if (!prepared) {
        BOOL ok = YES;
        for (NVMTLTexture *t in surfaces) if (![t nvmtlSurfaceOut]) ok = NO;
        return ok;
    }
    if (!result) {
        static _Atomic unsigned long long batches, copies;
        unsigned long long n = atomic_fetch_add(&batches, 1) + 1;
        unsigned long long total = atomic_fetch_add(&copies, roots.count) + roots.count;
        if (n == 1 || (n & 4095) == 0)
            nvlog("surface batch: %llu copies in %llu submissions (%llu waits removed)", total, n, total - n);
    }
    return result == 0;
}
