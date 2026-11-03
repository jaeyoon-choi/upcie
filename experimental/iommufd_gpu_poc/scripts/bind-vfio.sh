#!/bin/sh
# Bind one NVMe to vfio-pci in a translated (DMA-FQ) group and open the nodes
# the tests use. Usage: bind-vfio.sh <BDF>   (run as root on sid)
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
  echo "group has more than one device: $(ls $GRP/devices)"
  exit 1
}
for ns in /sys/class/nvme/*/; do
  [ "$(basename $(readlink -f $ns/device))" = "$BDF" ] || continue
  for b in $ns/nvme*n*; do
    n=$(basename $b)
    grep -q "^/dev/$n" /proc/mounts && {
      echo "$n is mounted"
      exit 1
    }
  done
done
modprobe vfio-pci
[ -e $DEV/driver ] && echo $BDF > $DEV/driver/unbind
echo DMA-FQ > $GRP/type 2> /dev/null || true
echo vfio-pci > $DEV/driver_override
echo $BDF > /sys/bus/pci/drivers_probe
echo "driver $(basename $(readlink $DEV/driver)), group $(basename $GRP) type $(cat $GRP/type), cdev $(ls $DEV/vfio-dev)"
