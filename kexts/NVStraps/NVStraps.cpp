// NVStraps: NvStrapsReBar (terminatorul) as a kext, so the stock NVRM.kext sees a large BAR1 on a Turing card.
// Loaded by OpenCore (Kernel > Add), it matches the GPU before NVRM, writes the BAR1 size straps in probe() and returns
// NULL, so the GPU goes on to NVRM. NVRM's placeLargeBar1() then finds the larger size in the Resizable BAR capability,
// resizes BAR1 and places it in the host bridge's 64-bit _CRS window.
// P8Z77-V LX (no Above 4G option): PCI0._CRS drops its 64-bit window [TUUD, +16 GB) while MM64 == 0. OpenCore
// ACPI > Patch on DSDT, Find oCORk01NNjQAkpRPU1lTC9MH, Replace oCORkwsBAAoCkwzTBwAAC9QH, makes
// If (MM64 == 0 || OSYS <= 0x7d3) false, so _CRS reports the window.
// Straps (envytools pstraps): BAR0 0x101000 bits 15:14 and 0x10100c bits 22:20, bit 31 = override.
// Boot-arg nvrmstraps=<n> asks for 64 MB << n (6 = 4 GB, the size for a GTX 1650); without it the kext does nothing.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>

#include <mach/kmod.h>

// OpenCore and kmutil place a kext only with a _kmod_info symbol (Xcode generates it; build.sh does not), as NMIntelFB
extern "C" kern_return_t _start(kmod_info_t *, void *) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *, void *)  { return KERN_SUCCESS; }
extern "C" { KMOD_EXPLICIT_DECL(com.apathane.NVStraps, "1.0.0", _start, _stop) }

#define LOG(fmt, ...) IOLog("NVStraps: " fmt "\n", ##__VA_ARGS__)

class NVStraps : public IOService {
    OSDeclareDefaultStructors(NVStraps)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
};
OSDefineMetaClassAndStructors(NVStraps, IOService)

static UInt32 rebarSizes(IOPCIDevice *pci)
{
    UInt32 cap = 0;
    for (UInt32 off = 0x100, n = 0; off >= 0x100 && off < 0x1000 && n < 64; n++) {
        UInt32 h = pci->extendedConfigRead32(off);
        if (h == 0 || h == 0xffffffffU) break;
        if ((h & 0xffff) == 0x15) { cap = off; break; }
        off = h >> 20;
    }
    if (!cap) return 0;
    UInt32 nbars = (pci->extendedConfigRead32(cap + 8) >> 5) & 7;
    for (UInt32 i = 0; i < nbars && i < 6; i++)
        if ((pci->extendedConfigRead32(cap + 8 + i * 8) & 7) == 1) return pci->extendedConfigRead32(cap + 4 + i * 8) >> 4;
    return 0;
}

IOService *NVStraps::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    UInt32 sel = 0;
    if (!pci || !PE_parse_boot_argn("nvrmstraps", &sel, sizeof sel)) return NULL;
    if (sel < 3 || sel > 10) { LOG("nvrmstraps=%u out of range 3-10 — ignored", sel); return NULL; }
    const UInt32 bar0 = pci->configRead32(0x10) & ~0xFU;
    if (!bar0 || !(pci->configRead16(0x04) & 0x2)) { LOG("BAR0 not decoding — skipped"); return NULL; }
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(bar0, 0x102000, kIODirectionInOut);
    IOMemoryMap *map = md ? md->map(kIOMapInhibitCache) : NULL;
    if (!map) { LOG("cannot map BAR0 0x%x — skipped", bar0); OSSafeReleaseNULL(md); return NULL; }
    volatile UInt32 *r = (volatile UInt32 *)map->getVirtualAddress();
    const UInt32 boot0 = r[0];
    volatile UInt32 *p0 = r + 0x101000 / 4, *p1 = r + 0x10100c / 4;
    const UInt32 old0 = *p0, old1 = *p1;
    if (((boot0 >> 24) & 0x1f) != 0x16) {
        LOG("BOOT_0 0x%08x is not Turing — skipped", boot0);
    } else {
        const UInt32 before = rebarSizes(pci);
        const UInt32 part1 = sel < 10 ? 2 : 3, part2 = sel < 10 ? sel - 2 : 7;
        const UInt32 new0 = (old0 & ~(3u << 14)) | part1 << 14 | 1u << 31, new1 = (old1 & ~(7u << 20)) | part2 << 20 | 1u << 31;
        if (new0 != old0) *p0 = new0;
        if (new1 != old1) *p1 = new1;
        const UInt32 after = rebarSizes(pci);
        if ((after >> (sel + 6)) & 1) {
            LOG("straps 0x%08x/0x%08x -> 0x%08x/0x%08x; BAR1 sizes 0x%x -> 0x%x (%u MB offered)", old0, old1, *p0, *p1, before, after, 64u << sel);
        } else {
            *p0 = old0; *p1 = old1;
            LOG("BAR1 sizes 0x%x -> 0x%x after the strap write, %u MB not offered — straps restored", before, after, 64u << sel);
        }
    }
    map->release(); md->release();
    return NULL;   // never claim the GPU: NVRM matches next
}
