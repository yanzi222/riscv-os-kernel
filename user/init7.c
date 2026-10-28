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
    //show Reading/writing a kernel page from user-mode

    
    char* a=(char*)kern;
    char error=*a;
    *a='a';
    _msgout(&error);
}