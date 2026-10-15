#!/bin/bash
# gtx1650: repack the official release with this branch's binaries, so install.sh, uninstall.sh and the 1401 app keep
# working. Usage:
#   tools/gtx1650-pack.sh nullmoth-nvidia-2.0.2.tar.gz NVRM=<bin> [NVAccel=<bin>] [NVRMFB=<bin>] [NVMTLDriver=<bin>]
# Each <bin> is a built Mach-O (the file that goes in Contents/MacOS). Writes nullmoth-nvidia-2.0.2-gtx1650.tar.gz;
# install it with: tar -xzf nullmoth-nvidia-2.0.2-gtx1650.tar.gz && cd pkgroot && sudo ./install.sh
set -euo pipefail
src=${1:?official tarball}; shift
out=${src%.tar.gz}-gtx1650.tar.gz
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
tar -xzf "$src" -C "$T"
P=$T/pkgroot; [ -f "$P/install.sh" ] || { echo "no pkgroot/install.sh in $src"; exit 2; }
for arg in "$@"; do
  name=${arg%%=*}; bin=${arg#*=}
  [ -f "$bin" ] || { echo "missing $bin"; exit 2; }
  case $name in
    NVRM|NVRMFB|NVRMAGDC) dst=("$P/Library/Extensions/$name.kext/Contents/MacOS/$name") ;;
    NVAccel) dst=("$P/Library/NullMoth/kexts/15/NVAccel.kext/Contents/MacOS/NVAccel")
             [ -f "$P/Library/Extensions/NVAccel.kext/Contents/MacOS/NVAccel" ] && dst+=("$P/Library/Extensions/NVAccel.kext/Contents/MacOS/NVAccel") ;;
    NVMTLDriver) dst=("$P/Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver") ;;
    *) echo "unknown component $name"; exit 2 ;;
  esac
  for d in "${dst[@]}"; do [ -f "$d" ] || { echo "$d not in the package"; exit 2; }; cp "$bin" "$d"; echo "replaced ${d#$T/}"; done
  case $name in
    NVMTLDriver) codesign -f -s - "$P/Library/GPUBundles/NVMTLDriver.bundle" ;;
    NVRM) codesign -f -s - "$P/Library/Extensions/NVRM.kext" ;;
  esac
done
# re-pin paired_kexts to the new binaries (the Extensions copy, as shipped)
python3 - "$P" <<'EOF'
import hashlib, json, os, sys
p = sys.argv[1]; f = os.path.join(p, "BASELINE-IDENTITY.json")
j = json.load(open(f))
for k in j.get("paired_kexts", {}):
    for b in (f"{p}/Library/Extensions/{k}.kext/Contents/MacOS/{k}", f"{p}/Library/NullMoth/kexts/15/{k}.kext/Contents/MacOS/{k}"):
        if os.path.isfile(b):
            j["paired_kexts"][k]["sha256"] = hashlib.sha256(open(b, "rb").read()).hexdigest(); break
json.dump(j, open(f, "w"), indent=2)
EOF
tar -czf "$out" -C "$T" pkgroot
echo "wrote $out"
