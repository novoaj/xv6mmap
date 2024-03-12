#include "types.h"
#include "stat.h"
#include "user.h"
#include "wmap.h"

int main(void) 
{
    int flags = MAP_ANONYMOUS | MAP_FIXED;
    printf(1, "flags from userprogram: %d\n", flags);
    uint address = wmap(0x60000000, 8192, flags, -1);
    printf(1,"return addr: %x\n", address);
    uint address2 = wmap(0x60000000, 8192, MAP_ANONYMOUS, -1);
    printf(1,"return addr: %x\n", address2);
    uint address3 = wmap(0x60000000, 10000, MAP_ANONYMOUS, -1);
    printf(1,"return addr: %x\n", address3);
    struct pgdirinfo* pdinfo;
    getpgdirinfo(&pdinfo);
    exit();
} 