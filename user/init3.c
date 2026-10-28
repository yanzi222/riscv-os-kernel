#include "syscall.h"
#include "string.h"
#include <stdint.h>

#define IOCTL_SETPOS 4

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
        _msgout("Child writing...\n");
        
        // Child writes three messages
        _write(fd, "Child msg 1\n", 11);
        _usleep(1000000);  // Sleep to ensure parent/child write separation
        _write(fd, "Child msg 2\n", 11);
        _usleep(1000000);
        _write(fd, "Child msg 3\n", 11);
        
        _close(fd);
        _exit();
    } else {
        // Parent process
        _msgout("Parent writing...\n");
        
        // Parent writes three messages
        _write(fd, "Parent msg 1\n", 12);
        _usleep(500000);  // Different sleep timing than child
        _write(fd, "Parent msg 2\n", 12);
        _usleep(500000);
        _write(fd, "Parent msg 3\n", 12);
        
        // Wait for child to finish
        _wait(pid);
        
        // Read and display file contents
        char buffer[1024];
        uint64_t pos = 0;
        _ioctl(fd, IOCTL_SETPOS, &pos);  // Reset to start of file
        int bytes = _read(fd, buffer, sizeof(buffer));
        if (bytes > 0) {
            buffer[bytes] = '\0';
            _msgout("File contents:\n");
            _msgout(buffer);
        }
        
        _close(fd);
        _exit();
    }
}
