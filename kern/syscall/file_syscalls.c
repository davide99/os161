#include <types.h>
#include <kern/unistd.h>
#include <lib.h>    //getch
#include <syscall.h>

ssize_t sys_read(int fd, userptr_t buf, size_t count){
    size_t i;
    char *ptr = (char*)buf;

    if (fd == STDIN_FILENO) {
        for(i=0; i<count; i++){
            *ptr = (char)getch();
            ptr++;
        }
    }

    return (ssize_t)(ptr-(char*)buf);
}

ssize_t sys_write(int fd, const userptr_t buf, size_t count) {
    size_t i;

    if (fd == STDOUT_FILENO) {
        for(i=0; i<count; i++){
            putch((int)*((char*)buf + i));
        }
    }

    return (ssize_t)(count);
}