SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

$SHELL_FOLDER/output/qemu/bin/qemu-system-riscv64 \
-M quard-star \
-m 1G \
-smp 8 \
-drive if=pflash,bus=0,unit=0,format=raw,\
file=$SHELL_FOLDER/output/lowlevelboot/fw.bin \
-nographic --parallel none

# if=pflash 告诉 QEMU，这不是普通硬盘镜像，而是并行 Flash 设备的后端文件
# -drive if=pflash的参数 将固件配置到模拟器的固件加载位置
