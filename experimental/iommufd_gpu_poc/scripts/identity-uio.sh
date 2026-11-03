#!/bin/sh
# Put one NVMe in an identity (untranslated) IOMMU domain under
# uio_pci_generic with bus mastering on, and reserve 64 hugepages.
# Usage: identity-uio.sh <BDF>   (root on sid). Undo with restore-nvme.sh.
set -e
BDF=$1
[ -n "$BDF" ] || {
  echo "usage: $0 <BDF>"
  exit 1
}
[ "$BDF" = 0000:4d:00.0 ] && {
  echo "refusing the dataset device"
  exit 1
}
DEV=/sys/bus/pci/devices/$BDF
GRP=/sys/kernel/iommu_groups/$(basename $(readlink $DEV/iommu_group))
[ "$(ls $GRP/devices | wc -l)" = 1 ] || {
  echo "group has more than one device"
  exit 1
}
[ -e $DEV/driver ] && echo $BDF > $DEV/driver/unbind
echo identity > $GRP/type
modprobe uio_pci_generic
echo uio_pci_generic > $DEV/driver_override
echo $BDF > /sys/bus/pci/drivers_probe
setpci -s $BDF COMMAND=0x0406
echo 64 > /proc/sys/vm/nr_hugepages
echo "driver $(basename $(readlink $DEV/driver)), group type $(cat $GRP/type), cmd $(setpci -s $BDF COMMAND), hugepages $(awk '/HugePages_Free/{print $2}' /proc/meminfo)"
