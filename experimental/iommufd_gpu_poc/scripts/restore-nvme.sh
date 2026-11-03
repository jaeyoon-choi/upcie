#!/bin/sh
# Return one NVMe to the kernel nvme driver in a DMA-FQ group, release hugepages.
# Usage: restore-nvme.sh <BDF>   (root on sid)
BDF=$1
[ -n "$BDF" ] || {
  echo "usage: $0 <BDF>"
  exit 1
}
DEV=/sys/bus/pci/devices/$BDF
GRP=/sys/kernel/iommu_groups/$(basename $(readlink $DEV/iommu_group))
[ -e $DEV/driver ] && echo $BDF > $DEV/driver/unbind
echo DMA-FQ > $GRP/type
echo > $DEV/driver_override
echo $BDF > /sys/bus/pci/drivers_probe
echo 0 > /proc/sys/vm/nr_hugepages
echo "driver $(basename $(readlink $DEV/driver)), group type $(cat $GRP/type)"
