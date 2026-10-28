#include "syscall.h"
#include "string.h"

void main(void) {
    // Open file
    int fd = _fsopen(1, "test.txt");
    if (fd < 0) {
        _msgout("Failed to open file\n");
        _exit();
    }

    // Fork into parent and child
    int pid = _fork();
    
    if (pid == 0) {
        // Child process
        char buffer[100];
        const char *msg = "Hello from child\n";
        
        // Read from file
        _read(fd, buffer, sizeof(buffer));
        _msgout(buffer);
        
        // Write to file
        _write(fd, msg, strlen(msg));
        
        // Close file and exit
        _close(fd);
        _exit();
    } else {
        // Parent process
        // Close file immediately - shouldn't affect child
        _close(fd);
        
        // Wait for child to finish
        _wait(pid);
        _exit();
    }
}
