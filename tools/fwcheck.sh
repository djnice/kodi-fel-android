#!/bin/bash
# Checks whether a dvfel.ko build fits the firmware of a Dune HD / R_volution
# player (adb, userdebug firmware: adb root).
# usage: fwcheck.sh <adb device, e.g. <player-ip>:5555> [dvfel.ko]
#
# The firmware kernel only WARNS about symbol CRC mismatches ("disagrees about
# version of symbol ...") and loads the module anyway, so the check is a trial
# load: insmod, look for new warnings, rmmod. No warnings = the kernel and
# Amlogic structures the module uses are unchanged (the CRCs cover them).
# Warnings list the changed symbols (Kodi FEL installs the module anyway): if
# it misbehaves there, check those types first (modver.py db/symvers + kbuild.sh; the firmware's
# vendor modules are pulled to ~/dunefw/fw/<firmware>_<model>/ for that, the
# aml_media.ko exports come from the firmware image's vendor_boot ramdisk).
set -e
DEV=$1
KO=${2:-$HOME/dvfel-android/dvfel/dvfel.ko}
ADB=${ADB:-$HOME/platform-tools/adb}
[ -n "$DEV" ] || { echo "usage: $0 <adb device> [dvfel.ko]"; exit 1; }
$ADB connect "$DEV" >/dev/null 2>&1 || true
$ADB -s "$DEV" root >/dev/null 2>&1 || true
sleep 3
$ADB connect "$DEV" >/dev/null 2>&1 || true
A="$ADB -s $DEV"
FSP=$($A shell "sed -n 's/^FSP=//p' /system/xbin/dunehd/init" | tr -d '\r')
FW=$($A shell "sed -n 's/^firmware_version = //p' ${FSP:-/data/data/com.dunehd.app}/config/last_fw_info.txt" | tr -d '\r')
MODEL=$($A shell getprop ro.product.model | tr -d '\r')
echo "device: $MODEL, firmware ${FW:-?}, kernel $($A shell uname -r | tr -d '\r'), FS prefix ${FSP:-?}"

OUT=~/dunefw/fw/${FW:-unknown}_$(echo "$MODEL" | tr ' /' '__')
mkdir -p "$OUT/modules"
$A shell cat /proc/version > "$OUT/proc_version.txt"
for m in $($A shell 'ls /vendor/lib/modules/*.ko' | tr -d '\r'); do
	$A pull "$m" "$OUT/modules/" >/dev/null 2>&1
done
echo "$(ls "$OUT/modules" | wc -l) vendor modules saved to $OUT"
vm_mod=$(modinfo -F vermagic "$KO")
vm_fw=$(modinfo -F vermagic "$(ls "$OUT"/modules/*.ko | head -1)")
[ "$vm_mod" = "$vm_fw" ] && echo "vermagic: match ($vm_mod)" || { echo "vermagic: MISMATCH ($vm_mod / $vm_fw)"; exit 2; }

$A push "$KO" /data/local/tmp/dvfel-check.ko >/dev/null
$A shell '
reload=
if [ -d /sys/module/dvfel ]; then
  rmmod dvfel || { echo "dvfel is in use (stop playback / Kodi FEL first)"; exit 3; }
  reload=1
fi
b=$(dmesg | grep -c "dvfel: disagrees about version")
if ! insmod /data/local/tmp/dvfel-check.ko; then
  echo "RESULT: insmod failed"; dmesg | grep -i dvfel | tail -5
else
  a=$(dmesg | grep -c "dvfel: disagrees about version")
  if [ "$a" -gt "$b" ]; then
    dmesg | grep "dvfel: disagrees about version" | tail -n $((a - b))
    echo "RESULT: $((a - b)) symbol versions differ (the module loads anyway; check these structures if it misbehaves)"
  else
    echo "RESULT: fits"
  fi
  rmmod dvfel
fi
rm -f /data/local/tmp/dvfel-check.ko
for s in /data/data/*/config/boot/dvfel.sh; do
  [ -n "$reload" ] && [ -x "$s" ] && sh "$s" && echo "(the installed module is loaded again)"
done
true'
