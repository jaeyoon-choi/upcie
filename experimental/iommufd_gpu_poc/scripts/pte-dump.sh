#!/bin/sh
# Print the first VT-d entries of the NVMe's translation table (needs
# CONFIG_INTEL_IOMMU_DEBUGFS). Run while pal_probe --hold keeps a mapping.
BDF=$1
cat /sys/kernel/debug/iommu/intel/$BDF/domain_translation_struct | sed -n '1,6p'
echo "entries: $(cat /sys/kernel/debug/iommu/intel/$BDF/domain_translation_struct | wc -l)"
