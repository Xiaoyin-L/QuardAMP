platform-objs-y += platform.o

FW_JUMP=y

# OpenSBI runs from DRAM after lowlevelboot copies it from pflash.
FW_TEXT_START=0x80000000

# The next S-mode stage is copied by lowlevelboot to this address.
FW_JUMP_ADDR=0x80200000
FW_JUMP_FDT_ADDR=0x82200000
