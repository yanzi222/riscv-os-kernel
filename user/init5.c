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
    //show demand paging by reading/writing

    
    char* a=(char*)USER;
    *a='e';
    
    while(1);

    //show *,& operations

    /*
    char* a=(char*)USER;
    *a='e';
    *(a+1)='c';
    *(a+2)='e';
    *(a+3)='3';
    *(a+4)='9';
    *(a+5)='1';
    *(a+6)=NULL;
    char b='x';
    char* c=&b;
    _msgout(a);
    _msgout(c);
    */

    //show Reading/writing a kernel page from user-mode

    /*
    char* a=(char*)kern;
    char error=*a;
    *a='a';
    */

    //show Reading from a page without read permissions/Writing to a page without write permissions

    /*
    char* a=(char*)NOTREAD;
    char error=*a;
    a=(char*)NOTWRITE;
    *a='s';
    */
}