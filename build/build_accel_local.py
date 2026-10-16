#!/usr/bin/env python3
"""Build NVAccel against the pinned NVIDIA 610.57.04 interfaces, without an RM build tree."""
import os, pathlib, subprocess, sys, plistlib, json
root=pathlib.Path(__file__).resolve().parents[1]
deps=pathlib.Path(sys.argv[1]).resolve(); out=pathlib.Path(sys.argv[2]).resolve()
# The deps checkout was re-imported locally (new git HEAD), so pin the release by version.mk instead of the upstream commit.
assert 'NVIDIA_VERSION = 610.57.04' in (deps/'version.mk').read_text(), 'Expected NVIDIA 610.57.04'
out.mkdir(parents=True,exist_ok=True)
sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
inc=['src/nvidia/arch/nvalloc/unix/include','src/nvidia-modeset/os-interface/include','src/nvidia-modeset/kapi/interface','src/nvidia-modeset/interface','src/nvidia-modeset/include','src/common/sdk/nvidia/inc','src/common/inc','src/common/unix/common/inc','src/common/modeset','src/common/unix/nvidia-push/interface','src/common/unix/common/utils/interface']
flags=['-arch','x86_64','-fapple-kext','-mkernel','-nostdinc','-I'+sdk+'/System/Library/Frameworks/Kernel.framework/Headers','-I'+str(root/'kexts/NVRM'),'-DKERNEL','-DKERNEL_PRIVATE','-DDRIVER_PRIVATE','-DAPPLE','-DNeXT','-std=c++17','-fno-rtti','-fno-exceptions','-fno-builtin','-fno-common','-Wall','-Wno-unused-parameter','-Wno-inconsistent-missing-override','-Wno-unused-function','-O2']+['-I'+str(deps/i) for i in inc]+['-I'+str(root/'kexts/NVRM/accel'/i) for i in ['.','iofam','re']]
if os.environ.get('NM_TAHOE'): flags.append('-DNM_TAHOE')   # NM_TAHOE=1: the macOS 26 accelerator, as build/accel_build.sh
cmd=['xcrun','clang++']+flags+['-c',str(root/'kexts/NVRM/accel/nvrm-accel.cpp'),'-o',str(out/'nvrm-accel.o')]
(out/'compile-command.json').write_text(json.dumps(cmd,indent=2))
with (out/'compile.log').open('w') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
if r.returncode:print((out/'compile.log').read_text()[-10000:]);sys.exit(r.returncode)
bundle=out/'NVAccel.kext/Contents';(bundle/'MacOS').mkdir(parents=True,exist_ok=True)
subprocess.run(['xcrun','clang++','-arch','x86_64','-fapple-kext','-nostdlib','-Xlinker','-kext','-lkmodc++','-lkmod','-lcc_kext',str(out/'nvrm-accel.o'),'-o',str(bundle/'MacOS/NVAccel')],check=True)
p=plistlib.loads((root/'kexts/NVRM/accel/Info.plist').read_bytes());(bundle/'Info.plist').write_bytes(plistlib.dumps(p,sort_keys=False))
subprocess.run(['codesign','--force','--sign','-',str(bundle.parent)],check=True)
print(bundle.parent)
