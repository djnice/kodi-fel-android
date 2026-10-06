#!/bin/bash
# Copy the Android FEL sources (and the Dune kernel module) into the Android
# Kodi tree (WSL) and build.
# usage: sync.sh [build|apk|release]
#   build/apk: debug build (~/kodi-android/kodi-build, org.xbmc.kodi)
#   release:   "Kodi FEL" release APK (~/kodi-android/kodi-build-rel,
#              org.xbmc.kodi.fel), signed with ~/.android/kodi-fel.keystore
set -e
K=~/kodi-android/xbmc
V=$K/xbmc/cores/VideoPlayer/DVDCodecs/Video
SRC=/mnt/c/dovi_s5/dvfel
# felgpu: the released CoreELEC copy (patches/kodi carries the same)
CEV=$(ls -d ~/ce22/CoreELEC/build.*/build/kodi-*/xbmc/cores/VideoPlayer/DVDCodecs/Video | head -1)
for f in AndroidDvFel.cpp AndroidDvFel.h; do sed 's/\r$//' $SRC/android/kodi/$f > $V/$f; done
sed 's/\r$//' $SRC/kernel/dvfel_uapi.h > $V/dvfel_uapi.h
cp $CEV/felgpu.c $CEV/felgpu.h $V/
# the kernel module builds for the Dune HD 5.4 GKI firmware kernels, one per
# CRC set (android/tools/kbuild.sh, fwcheck.sh); the installer tries them in
# the order of variants.txt
mkdir -p $K/system/dvfel
rm -f $K/system/dvfel/*
cp ~/dvfel-android/dvfel/dvfel.ko $K/system/dvfel/dvfel-r24-260303.ko
printf '# dvfel.ko builds, tried in this order\ndvfel-r24-260303.ko\n' > $K/system/dvfel/variants.txt
set +e	# the build steps report their own status
case "$1" in
  build) cd ~/kodi-android/kodi-build && make -j14 2>&1 | grep -E " error|error:|Error [0-9]" | head -30; echo "make exit ${PIPESTATUS[0]}" ;;
  apk) cd ~/kodi-android/kodi-build && make -j14 >/tmp/kmake.log 2>&1; r=$?; grep -E " error" /tmp/kmake.log | head -30; echo "make exit $r"
       [ $r = 0 ] && { make apk >/tmp/kapk.log 2>&1; echo "apk exit $?"; ls -la $K/kodiapp-armeabi-v7a-debug.apk; } ;;
  release)
       B=~/kodi-android/kodi-build-rel
       if [ ! -f $B/CMakeCache.txt ]; then
         mkdir -p $B && cd $B
         ~/android-tools/xbmc-depends/x86_64-linux-gnu-native/bin/cmake $K -DCMAKE_TOOLCHAIN_FILE=$HOME/android-tools/xbmc-depends/arm-linux-androideabi-24-release/share/Toolchain.cmake \
               -DAPP_PACKAGE=org.xbmc.kodi.fel >/tmp/kcmake-rel.log 2>&1 || { tail -20 /tmp/kcmake-rel.log; exit 1; }
       fi
       cd $B && make -j14 >/tmp/kmake-rel.log 2>&1; r=$?; grep -E " error" /tmp/kmake-rel.log | head -30; echo "make exit $r"
       [ $r = 0 ] || exit 1
       export KODI_ANDROID_STORE_FILE=$HOME/.android/kodi-fel.keystore
       export KODI_ANDROID_STORE_PASSWORD=$(cat $HOME/.android/kodi-fel.pass)
       export KODI_ANDROID_KEY_ALIAS=kodifel
       export KODI_ANDROID_KEY_PASSWORD=$KODI_ANDROID_STORE_PASSWORD
       make apk >/tmp/kapk-rel.log 2>&1; echo "apk exit $?"; ls -la $K/kodiapp-armeabi-v7a-release.apk ;;
esac
