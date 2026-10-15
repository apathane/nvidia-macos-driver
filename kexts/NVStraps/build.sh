#!/bin/bash
# Build NVStraps.kext with only the Command Line Tools: xcode-select --install, then kexts/NVStraps/build.sh
# Output: kexts/NVStraps/out/NVStraps.kext -> copy to EFI/OC/Kexts and add it under Kernel > Add in config.plist.
# Flags follow build/build_agdc.sh; _start/_stop/_kmod_info come from NVStraps.cpp, so no -lkmod.
set -euo pipefail
cd "$(dirname "$0")"
SDK=$(xcrun --show-sdk-path); K=out/NVStraps.kext/Contents
rm -rf out; mkdir -p $K/MacOS
xcrun clang++ -arch x86_64 -fapple-kext -mkernel -nostdinc -I"$SDK/System/Library/Frameworks/Kernel.framework/Headers" \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common \
  -Wall -O2 -c NVStraps.cpp -o out/NVStraps.o
xcrun clang++ -arch x86_64 -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lcc_kext out/NVStraps.o -o $K/MacOS/NVStraps
cp Info.plist $K/Info.plist
nm $K/MacOS/NVStraps | grep -q _kmod_info || { echo "STOP: no _kmod_info"; exit 1; }
echo "built $(pwd)/out/NVStraps.kext"
