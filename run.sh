SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

DEFAULT_VC="960x480"
QEMU="$SHELL_FOLDER/output/qemu/bin/qemu-system-riscv64"
FW="$SHELL_FOLDER/output/fw/fw.bin"
FS_IMG="$SHELL_FOLDER/xv6-riscv/fs.img"

if [ ! -x "$QEMU" ]; then
    echo "missing QEMU: $QEMU"
    echo "please run ./build.sh first"
    exit 1
fi

if [ ! -f "$FW" ] || [ ! -f "$FS_IMG" ]; then
    echo "missing firmware or xv6 file system image"
    echo "please run ./build.sh first"
    exit 1
fi

"$QEMU" \
    -M quard-star \
    -m 1G \
    -smp 8 \
    -global virtio-mmio.force-legacy=false \
    -drive if=pflash,bus=0,unit=0,format=raw,file="$FW" \
    -drive if=none,file="$FS_IMG",format=raw,id=x0 \
    -device virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0 \
    --serial "vc:${DEFAULT_VC}" \
    --serial "vc:${DEFAULT_VC}" \
    --serial "vc:${DEFAULT_VC}" \
    --monitor "vc:${DEFAULT_VC}" \
    --parallel none

    
# if=pflash 告诉 QEMU，这不是普通硬盘镜像，而是并行 Flash 设备的后端文件
# -drive if=pflash的参数 将固件配置到模拟器的固件加载位置
