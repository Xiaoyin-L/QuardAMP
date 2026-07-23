set -e
SHELL_FOLDER=$(cd "$(dirname "$0")";pwd)

cd "$SHELL_FOLDER/qemu-10.2.4"
if [ ! -f "build/config-host.mak" ]; then
    ./configure --prefix="$SHELL_FOLDER/output/qemu" --target-list=riscv64-softmmu --enable-gtk --enable-virtfs --disable-gio
fi
make -j$(nproc)
make install
cd "$SHELL_FOLDER"
