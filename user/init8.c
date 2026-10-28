#include "syscall.h"
#include "string.h"
#define USER 0xc0008000
#define NOTREAD 0xc1008000
#define NOTWRITE 0xc2008000
#define kern 0x80000000
void main(void) {

    int result;

    // Open ser1 device as fd=0
    result = _devopen(0, "ser", 1);
    if (result < 0)
        return;
    //show Reading from a page without read permissions/Writing to a page without write permissions
    char* a=(char*)NOTREAD;
    _msgout(a);
    a=(char*)NOTWRITE;
    *a='s';
    *(a+1)=0x00;
    _msgout(a);
}