#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

// tests fork, copy metadata, copy file, init_metadata
void 
test_fork()
{
    int pid;
    int parentpid = getpid();
    if ((pid = fork()) > 0)
    {
        procdump(parentpid,pid);
    }
    else if (pid == 0)
    {
        
    }
    else
    {
        printf("fork failed\n");
    }   
}

void add_test();()
{
    int pid = getpid();
    procdump(pid,pid);
    char* buffer = malloc (4096*16);
    procdump(pid,pid);
}

void init_test()
{
    int pid = getpid();
    procdump(pid,pid);
}
int
main(int argc, char *argv[])
{   
    init_test();

    test_fork();

    add_test();
    return 0;
}