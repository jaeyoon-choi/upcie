#!/bin/sh
# Follow-up runs for one NVMe: correctness, throughput, map cost, lifecycle.
# Usage: followup-run.sh <BDF> <SLBA> [parts]   (run as root, PAL or gmem kernel)
#   parts: any of c bench map life iova pte (default: c bench map life)
# Env: SECS (10), SPAN (bytes overwritten by write benches, 1 GiB),
#      XFER_BIG (131072), WR (1; 0 skips the write benches), PAL_NVME (./pal_nvme),
#      ORDER ("pal pa host": bench path order), SETTLE (30 s after the fill),
#      GPU (pal; gmem maps the GPU through a guest_memfd instead of the dma-buf)
# Writes LBAs SLBA .. SLBA + SPAN/lba_size - 1 when WR=1, else SLBA .. +383.
set -u
BDF=$1
SLBA=$2
PARTS=${3:-"c bench map life"}
SECS=${SECS:-10}
SPAN=${SPAN:-1073741824}
XFER_BIG=${XFER_BIG:-131072}
WR=${WR:-1}
ORDER=${ORDER:-"pal pa host"}
SETTLE=${SETTLE:-30}
P=${PAL_NVME:-./pal_nvme}
GPU=${GPU:-pal}
S=$(dirname "$0")

[ "$BDF" = 0000:4d:00.0 ] && {
  echo "refusing the dataset device"
  exit 1
}
others=$(nvidia-smi --query-compute-apps=pid --format=csv,noheader | wc -l)
[ "$others" = 0 ] || {
  echo "GPU has $others other compute process(es); the lifecycle and bench runs need it idle"
  exit 1
}
modprobe vfio-pci
modprobe dmabuf_import
modprobe iommu_map_pa
case $GPU in
pal) G="" ;;
gmem)
  G="--gmem"
  modprobe kvm_intel
  modprobe gpu_gmem_provider || exit 1
  ;;
*)
  echo "GPU must be pal or gmem"
  exit 1
  ;;
esac
sh "$S/bind-vfio.sh" "$BDF" || exit 1
VF=/dev/vfio/devices/$(ls /sys/bus/pci/devices/$BDF/vfio-dev)
echo "== $(date) $(uname -r) bdf=$BDF slba=$SLBA secs=$SECS span=$SPAN xfer_big=$XFER_BIG wr=$WR order=$ORDER gpu=$GPU"
echo "== path: $(readlink -f /sys/bus/pci/devices/$BDF) numa=$(cat /sys/bus/pci/devices/$BDF/numa_node)"
dmesg -C

run() {
  echo "#### $*"
  $P --vfio "$VF" --slba "$SLBA" "$@"
  echo "rc=$?"
}

for part in $PARTS; do
  case $part in
  c)
    run $G
    run $G --xfer 4096
    run --map-pa "$BDF"
    ;;
  bench)
    # A read right after a write bench runs while the drive is still busy
    # with the writes, so fill the span once, let it settle, run every read
    # before any write, and take the paths in $ORDER. --target host still
    # maps the GPU buffer, so it needs $G on the gmem kernel.
    if [ "$WR" = 1 ]; then
      run $G --target host --bench wr --qd 32 --xfer "$XFER_BIG" --secs 3 --span "$SPAN"
      sleep "$SETTLE"
    fi
    for dir in rd wr; do
      [ $dir = wr ] && [ "$WR" = 0 ] && continue
      for path in $ORDER; do
        case $path in
        pal | gmem) M="$G" ;;
        pa) M="--map-pa $BDF" ;;
        host) M="$G --target host" ;;
        esac
        run $M --bench $dir --qd 1 --xfer 4096 --secs "$SECS" --span "$SPAN"
        run $M --bench $dir --qd 32 --xfer "$XFER_BIG" --secs "$SECS" --span "$SPAN"
      done
    done
    ;;
  iova)
    # Separate the mapping method from the IOVA: each path at its own
    # default IOVA and at the other path's, read-only, twice in reverse order.
    for order in "a b c d host" "host d c b a"; do
      for v in $order; do
        case $v in
        a) M="$G" ;;
        b) M="$G --gpu-iova 0x4000000000" ;;
        c) M="--map-pa $BDF" ;;
        d) M="--map-pa $BDF --gpu-iova 0x40000000" ;;
        host) M="$G --target host" ;;
        esac
        run $M --bench rd --qd 32 --xfer "$XFER_BIG" --secs "$SECS" --span "$SPAN"
      done
    done
    ;;
  pte)
    # VT-d leaf entries of the GPU mapping while a read bench holds it. A
    # leaf with the PS bit (0x80) maps 2 MiB. The host buffer is the only
    # mapping below IOVA 0xa00000 (debugfs prints the IOVA PFN 15 chars wide).
    T=/sys/kernel/debug/iommu/intel/$BDF/domain_translation_struct
    for M in "$G" "--map-pa $BDF"; do
      echo "#### pte $M"
      $P --vfio "$VF" --slba "$SLBA" $M --bench rd --qd 32 --xfer "$XFER_BIG" --secs 8 --span "$SPAN" | grep -E 'gpu:|bench result' &
      sleep 4
      awk 'NR > 2 {
             if ($1 < "0x0000000000a00") { host++; next }
             if (index("89abcdef", substr($NF, length($NF) - 1, 1))) big++; else small++
             low[substr($NF, length($NF) - 2)]++
             if (big + small <= 2) print }
           END { printf "gpu leaves: 2 MiB %d, 4 KiB %d, low bits:", big, small
                 for (k in low) printf " %s x%d", k, low[k]; printf ", host 4 KiB leaves: %d\n", host }' "$T"
      wait
    done
    ;;
  map)
    run $G --map-loop 1000 --no-negative
    run --map-pa "$BDF" --map-loop 1000 --no-negative
    ;;
  life)
    echo "== cuda used before: $(nvidia-smi --query-gpu=memory.used --format=csv,noheader)"
    run $G --lifecycle close --no-negative
    run --map-pa "$BDF" --lifecycle close --no-negative
    run $G --lifecycle free
    run --map-pa "$BDF" --lifecycle free
    echo "#### kill -9 during a read bench"
    $P --vfio "$VF" --slba "$SLBA" $G --bench rd --qd 32 --secs 30 --span "$SPAN" &
    pid=$!
    sleep 8
    kill -9 $pid
    wait $pid
    echo "killed rc=$?"
    sleep 2
    run $G --no-negative
    echo "== cuda used after: $(nvidia-smi --query-gpu=memory.used --format=csv,noheader)"
    ;;
  esac
done

echo "#### dmesg"
dmesg | grep -v -E 'added peer-to-peer|vfio-pci .*reset' | tail -60
echo "== done $(date)"
