#!/usr/bin/env python3
"""VRAM re-import heals a cookie-desynced surface texture in place.

After a BAR1 unwire/re-wire cycle the kernel works on a fresh zeroed allocation
while the texture still draws the old one: dirtys come back NO_CACHE, flips go
stale, and the panel ping-pongs. nvmtlSurfaceOut must then rebind the texture to
the current allocation (re-handshaking the kernel cookie on import) and push the
preserved pixels there. Failure must keep the old binding, never brick it.
"""
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
source = (root / 'plugin/NVMTLObjects.m').read_text()
sync_start = source.index('static id nvmtl_surface_sync(')
sync_end = source.index('- (NSUInteger)width ', sync_start)
span1 = source[sync_start:sync_end]
ri_start = source.index('- (BOOL)nvmtlReimportVRAM {')
ri_end = source.index('- (uint32_t)swizzleKey ', ri_start)
span2 = source[ri_start:ri_end]
methods = span1 + span2
fixture = r'''
#import <Foundation/Foundation.h>
#include <assert.h>
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#import <objc/runtime.h>
typedef struct __IOSurface { unsigned seed, planes, id; size_t row, height; unsigned char cpu[64]; } Surface;
typedef Surface *IOSurfaceRef;
typedef int IOReturn;
typedef int MTLPixelFormat;
#define MTLTextureUsageShaderWrite 2
enum { kIOSurfaceLockReadOnly = 1 };
typedef struct { unsigned w,h,bpp,storage,placed; unsigned char gpu[64]; int gen; } MockImage;
typedef MockImage Image;
typedef MockImage nvk_image;
typedef struct { int id, dead; } nvk_buffer;
static unsigned dirty_mode, import_calls, alias_calls, uploads, readbacks;
static unsigned long long g_surfin_skip, g_surfin_copy;
static int import_fail, alias_fail, pixfmt_fail;
static int next_buf = 100, next_gen = 1000;
static unsigned long long tag_seed;
#define NVMTL_SURF_NO_CACHE 0x80000004u
static uint32_t IOSurfaceGetSeed(IOSurfaceRef s) { return s->seed; }
static unsigned IOSurfaceGetID(IOSurfaceRef s) { return s->id; }
static unsigned IOSurfaceGetPlaneCount(IOSurfaceRef s) { return s->planes; }
static size_t IOSurfaceGetBytesPerRow(IOSurfaceRef s) { return s->row; }
static size_t IOSurfaceGetHeight(IOSurfaceRef s) { return s->height; }
static IOReturn IOSurfaceLock(IOSurfaceRef s, unsigned opts, uint32_t *seed) {
    if (seed) *seed=s->seed; return 0;
}
static IOReturn IOSurfaceUnlock(IOSurfaceRef s, unsigned opts, uint32_t *seed) {
    if (!opts) s->seed++; if (seed) *seed=s->seed; return 0;
}
static void *IOSurfaceGetBaseAddressOfPlane(IOSurfaceRef s, uint32_t p) { return s->cpu; }
static size_t IOSurfaceGetBytesPerRowOfPlane(IOSurfaceRef s, uint32_t p) { return s->row; }
static size_t IOSurfaceGetWidthOfPlane(IOSurfaceRef s, uint32_t p) { return 2; }
static size_t IOSurfaceGetBytesPerElementOfPlane(IOSurfaceRef s, uint32_t p) { return 4; }
static size_t IOSurfaceGetHeightOfPlane(IOSurfaceRef s, uint32_t p) { return 2; }
static uint32_t nvmtl_vram_gen(IOSurfaceRef s, NSUInteger p) { return 1; }
static int nvmtl_vram_tag_current(IOSurfaceRef s, NSUInteger p, unsigned seed) { return 0; }
static void nvmtl_vram_tag_set(IOSurfaceRef s, NSUInteger p, unsigned seed) { tag_seed=seed; }
static unsigned nvmtl_vram_tag_bump(IOSurfaceRef s, NSUInteger p, unsigned seed) { return 1; }
static int nvmtl_vk_surface_dirty(unsigned s, unsigned p) { return (int)dirty_mode; }
static int nvmtl_noseed(void) { return 0; }
static void nvmtl_frame_dump(const char *tag, void *base, size_t row, unsigned w, unsigned h) {}
#define nvlog(...) ((void)0)
static int nvmtl_vk_image_write(void *q, nvk_image *i, const void *src, size_t row) {
    memcpy(i->gpu,src,16); uploads++; return 0;
}
static int nvmtl_vk_image_read(void *q, nvk_image *i, void *dst, size_t row) {
    memcpy(dst,i->gpu,16); readbacks++; return 0;
}
static int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8) {
    if (pixfmt_fail) return -1; *vk=0; *bpp=4; *a8=0; return 0;
}
static int nvmtl_vk_surface_vram(unsigned s, unsigned p, size_t need, nvk_buffer *out) {
    import_calls++; if (import_fail) return -1; out->id=next_buf++; out->dead=0; return 0;
}
static int nvmtl_vk_image_create_buffer_alias(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8,
        int storage, int render, const nvk_buffer *b, size_t off, size_t pitch, nvk_image *out) {
    alias_calls++; if (alias_fail) return -1; out->w=w; out->h=h; out->gen=next_gen++; return 0;
}
static void nvmtl_vk_image_destroy(nvk_image *i) { i->gen=-1; }
static void nvmtl_vk_buffer_destroy(nvk_buffer *b) { b->dead=1; }
@interface NVMTLTexture : NSObject {
@public NVMTLTexture *_parent; IOSurfaceRef _surf; BOOL _vramOn, _surfSynced;
    uint32_t _surfSeed, _surfGen; NSUInteger _plane; MockImage _i; void *_q;
    uint64_t _reimportTime; int _nviews; void *_ownView, *_swzView, *_sampledDepthView;
    int _fmt; NSUInteger _usage; BOOL _stex; nvk_buffer _vramBuf;
}
- (BOOL)nvmtlSurfaceCopyable;
- (void)nvmtlSurfaceIn;
- (BOOL)nvmtlSurfaceOut;
- (BOOL)nvmtlReimportVRAM;
@end
@implementation NVMTLTexture
- (BOOL)nvmtlSurfaceCopyable { return YES; }
'''
checks = r'''
@end
static NVMTLTexture *mktex(Surface *s) {
    NVMTLTexture *t=[NVMTLTexture new];
    t->_surf=s; t->_plane=0; t->_vramOn=YES;
    t->_i=(MockImage){.w=2,.h=2,.bpp=4,.gpu={9,9,9,9},.gen=1};
    t->_vramBuf=(nvk_buffer){.id=7,.dead=0};
    return t;
}
int main(void) { @autoreleasepool {
    // 1. NO_CACHE heals: readback preserves pixels, binding swaps, upload lands.
    { Surface s={.seed=10,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s);
      dirty_mode=NVMTL_SURF_NO_CACHE;
      unsigned ic0=import_calls;
      assert([t nvmtlSurfaceOut]);
      assert(import_calls==ic0+1);
      assert(t->_vramOn && t->_i.gen>=1000 && t->_vramBuf.id>=100);
      assert(uploads>=1 && readbacks>=1);
      const unsigned char px[4]={9,9,9,9};
      assert(!memcmp(t->_i.gpu,px,4)); }
    // 2. Immediate retry is rate-limited (no second import within 2 s).
    { Surface s={.seed=20,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s);
      dirty_mode=NVMTL_SURF_NO_CACHE;
      unsigned ic0=import_calls;
      assert([t nvmtlSurfaceOut]); assert(import_calls==ic0+1);
      assert([t nvmtlSurfaceOut]); assert(import_calls==ic0+1); }
    // 3. Cooldown expiry allows one more attempt.
    { Surface s={.seed=30,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s);
      t->_reimportTime=clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)-3000000000ull;
      dirty_mode=NVMTL_SURF_NO_CACHE;
      unsigned ic0=import_calls;
      assert([t nvmtlSurfaceOut]); assert(import_calls==ic0+1); }
    // 4. Clean dirty takes the skip path: no import, no upload.
    { Surface s={.seed=40,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s);
      dirty_mode=0;
      unsigned ic0=import_calls, up0=uploads;
      assert([t nvmtlSurfaceOut]);
      assert(import_calls==ic0 && uploads==up0); }
    // 5. Other errors read back but never re-import.
    { Surface s={.seed=50,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s);
      dirty_mode=0x80000005u;
      unsigned ic0=import_calls, rb0=readbacks;
      assert([t nvmtlSurfaceOut]);
      assert(import_calls==ic0 && readbacks==rb0+1 && t->_i.gen==1); }
    // 6. Live views veto the swap; old binding kept.
    { Surface s={.seed=60,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s); t->_nviews=1;
      assert(![t nvmtlReimportVRAM]);
      assert(t->_i.gen==1 && t->_vramBuf.id==7); }
    // 7. Failed import keeps the old binding intact.
    { Surface s={.seed=70,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s); import_fail=1;
      assert(![t nvmtlReimportVRAM]);
      assert(t->_vramOn && t->_i.gen==1 && t->_vramBuf.id==7 && !t->_vramBuf.dead);
      import_fail=0; }
    // 8. Non-VRAM textures never re-import.
    { Surface s={.seed=80,.planes=1,.id=7,.row=32,.height=2};
      NVMTLTexture *t=mktex(&s); t->_vramOn=NO;
      unsigned ic0=import_calls;
      assert(![t nvmtlReimportVRAM] && import_calls==ic0); }
    puts("PASS: NO_CACHE re-imports once per cooldown, preserves pixels, keeps old binding on failure");
} return 0; }
'''
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory)
    (path / 'test.m').write_text(fixture + methods + checks)
    subprocess.run(['clang', '-fobjc-arc', '-framework', 'Foundation',
                    str(path / 'test.m'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
