set -e
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)
CROSS_PREFIX=/opt/riscv64-lp64d--glibc--stable-2025.08-1/bin/riscv64-linux
XV6_TOOLPREFIX=riscv64-linux-gnu-

# 编译qemu
cd "$SHELL_FOLDER/qemu-10.2.4"
if [ ! -f "build/config-host.mak" ]; then
    ./configure --prefix="$SHELL_FOLDER/output/qemu" --target-list=riscv64-softmmu --enable-gtk --enable-virtfs --disable-gio
fi
make -j$(nproc)
make install
cd "$SHELL_FOLDER"


# 编译lowlevelboot
# 创建固件输出目录 用于存放生成的目标文件、ELF、BIN
if [ ! -d "$SHELL_FOLDER/output/lowlevelboot" ]; then  
mkdir $SHELL_FOLDER/output/lowlevelboot
fi  

cd lowlevelboot
$CROSS_PREFIX-gcc -x assembler-with-cpp -c startup.s -o $SHELL_FOLDER/output/lowlevelboot/startup.o
$CROSS_PREFIX-gcc -nostartfiles -T./boot.lds -Wl,-Map=$SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.map -Wl,--gc-sections $SHELL_FOLDER/output/lowlevelboot/startup.o -o $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.elf
$CROSS_PREFIX-objcopy -O binary -S $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.elf $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.bin
$CROSS_PREFIX-objdump --source --demangle --disassemble --reloc --wide $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.elf > $SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.lst

# 编译opensbi
if [ ! -d "$SHELL_FOLDER/output/opensbi" ]; then  
mkdir $SHELL_FOLDER/output/opensbi
fi  
cd $SHELL_FOLDER/opensbi
make CROSS_COMPILE=$CROSS_PREFIX- PLATFORM=quard_star FW_TEXT_START=0x80000000 FW_JUMP_ADDR=0x82000000 FW_JUMP_FDT_ADDR=0x82200000
cp -r $SHELL_FOLDER/opensbi/build/platform/quard_star/firmware/*.bin $SHELL_FOLDER/output/opensbi/

# 生成sbi.dtb
cd $SHELL_FOLDER/dts
dtc -I dts -O dtb -o $SHELL_FOLDER/output/opensbi/quard_star_sbi.dtb quard_star_sbi.dts

# 编译trusted_domain
if [ ! -d "$SHELL_FOLDER/output/trusted_domain" ]; then  
mkdir $SHELL_FOLDER/output/trusted_domain
fi  
cd $SHELL_FOLDER/trusted_domain
$CROSS_PREFIX-gcc -x assembler-with-cpp -c startup.s -o $SHELL_FOLDER/output/trusted_domain/startup.o
$CROSS_PREFIX-gcc -nostartfiles -T./link.lds -Wl,-Map=$SHELL_FOLDER/output/trusted_domain/trusted_fw.map -Wl,--gc-sections $SHELL_FOLDER/output/trusted_domain/startup.o -o $SHELL_FOLDER/output/trusted_domain/trusted_fw.elf
$CROSS_PREFIX-objcopy -O binary -S $SHELL_FOLDER/output/trusted_domain/trusted_fw.elf $SHELL_FOLDER/output/trusted_domain/trusted_fw.bin
$CROSS_PREFIX-objdump --source --demangle --disassemble --reloc --wide $SHELL_FOLDER/output/trusted_domain/trusted_fw.elf > $SHELL_FOLDER/output/trusted_domain/trusted_fw.lst

# 编译xv6
# xv6 使用系统 riscv64-linux-gnu 工具链（与 Buildroot 工具链前缀不同，互不冲突）
# 产出 kernel.bin 链接在 0x82000000，与 DTS 中 untrusted-domain next-addr 一致
if [ ! -d "$SHELL_FOLDER/output/xv6" ]; then
mkdir $SHELL_FOLDER/output/xv6
fi

cd $SHELL_FOLDER/xv6-riscv
make clean
make TOOLPREFIX=$XV6_TOOLPREFIX kernel/kernel fs.img
${XV6_TOOLPREFIX}objcopy -O binary -S kernel/kernel $SHELL_FOLDER/output/xv6/kernel.bin
${XV6_TOOLPREFIX}objdump --source --demangle --disassemble --reloc --wide kernel/kernel > $SHELL_FOLDER/output/xv6/kernel.lst


# 合成firmware固件
if [ ! -d "$SHELL_FOLDER/output/fw" ]; then  
mkdir $SHELL_FOLDER/output/fw
fi  
cd $SHELL_FOLDER/output/fw

rm -rf fw.bin
dd of=fw.bin bs=1k count=32k if=/dev/zero
dd of=fw.bin bs=1k conv=notrunc seek=0 if=$SHELL_FOLDER/output/lowlevelboot/lowlevel_fw.bin
dd of=fw.bin bs=1k conv=notrunc seek=512 if=$SHELL_FOLDER/output/opensbi/quard_star_sbi.dtb
dd of=fw.bin bs=1k conv=notrunc seek=2K if=$SHELL_FOLDER/output/opensbi/fw_jump.bin
dd of=fw.bin bs=1k conv=notrunc seek=4K if=$SHELL_FOLDER/output/trusted_domain/trusted_fw.bin
dd of=fw.bin bs=1k conv=notrunc seek=8K if=$SHELL_FOLDER/output/xv6/kernel.bin
cd $SHELL_FOLDER
