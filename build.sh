SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

cd qemu-10.2.4
if [ ! -d "$SHELL_FOLDER/output/qemu" ]; then  
./configure --prefix=$SHELL_FOLDER/output/qemu  --target-list=riscv64-softmmu --enable-gtk  --enable-virtfs --disable-gio
fi  
make -j4
make install
cd ..
