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
    //show *,& operations

    
    char* a=(char*)USER;
    *a='e';
    *(a+1)='c';
    *(a+2)='e';
    *(a+3)='3';
    *(a+4)='9';
    *(a+5)='1';
    *(a+6)=0x00;
    char b='x';
    char* c=&b;
    *(c+1)=0x00;
    _msgout(a);
    _msgout(&b);
    
}