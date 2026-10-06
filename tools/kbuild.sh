#!/bin/bash
# Build an external module for the Dune 5.4.210 GKI kernel.
# usage: kbuild.sh <module dir (WSL path)> [make targets]
# Compiler: clang 11.0.1 (the Dune kernel: AOSP clang r383902 = 11.0.1, LTO + CFI + SCS), LLVM=1.
# Headers: reconstructed Amlogic headers in $DSRC (layouts verified against the Dune binaries).
# CRCs: modpost takes them from ~/dunefw/Module.symvers.dune, generated from the
# __versions/__crc_* of the Dune modules (modver.py db + symvers).
set -e
M=$(readlink -f "$1"); shift || true
TOOLS=$(dirname $(readlink -f $0))
DSRC=~/dune54/linux-amlogic-a3aaaa5b0e646dcd8cf4a23d6b8d66a5292840eb
DOUT=~/dune54/outc
SYMVERS=~/dunefw/Module.symvers.dune
export LD_LIBRARY_PATH=~/toolchains/compat
export PATH=~/toolchains/clang11/bin:$(ls -d ~/ce22/CoreELEC/build.*/toolchain/bin):$PATH
if [ ! -f $SYMVERS ]; then
	python3 $TOOLS/modver.py symvers ~/dunefw/dune_crc.json $SYMVERS \
		~/dunefw/img/vbr/lib/modules/*.ko ~/dunefw/mods/modules/*.ko
fi
DCFG=$DSRC/arch/arm64/configs/meson64_gki_module_config
PRE="$(for y in $(grep "^CONFIG_.*=y" $DCFG | sed 's/=y//'); do echo -n " -D$y"; done; for m in $(grep "^CONFIG_.*=m" $DCFG | sed 's/=m//'); do echo -n " -D${m}_MODULE"; done)"
make -C $DSRC O=$DOUT ARCH=arm64 LLVM=1 CC=clang LD=ld.lld NM=llvm-nm OBJCOPY=llvm-objcopy AR=llvm-ar \
	OBJDUMP=llvm-objdump STRIP=llvm-strip CLANG_TRIPLE=aarch64-linux-gnu- CROSS_COMPILE=aarch64-libreelec-linux-gnu- \
	M=$M KCFLAGS="$PRE" KBUILD_EXTRA_SYMBOLS=$SYMVERS "${@:-modules}"
for ko in $M/*.ko; do
	n=$(python3 $TOOLS/modver.py show $ko | wc -l)
	echo "$(basename $ko): $n versioned imports, $(modinfo -F vermagic $ko)"
done
