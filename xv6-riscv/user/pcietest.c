#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  (void)argc;
  (void)argv;

  printf("pcietest: run PCIe accelerator DMA/MSI selftest\n");
  if(pcieacceltest() < 0){
    printf("pcietest: failed\n");
    exit(1);
  }
  printf("pcietest: passed\n");
  exit(0);
}
