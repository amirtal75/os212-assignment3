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

void add_test()
{
    int pid = getpid();
    procdump(pid,pid);
    char* buffer = malloc (4096*15);
    for (int i = 0; i < 10; i++)
    {
        *(buffer + (4096*i)) = (49 + i);
        *(buffer + (4096*i)) = (48 + i);
        printf("\n\nstart test: %d\n\n",i+1);
        procdump(pid,pid);
        printf("\nend test: %d\n\n\n",i+1);
    }
    
}

void init_test()
{
    printf("before\n");
    int pid = getpid();
    printf("parent pid: %d\n", pid);
    procdump(pid,pid);
}
int
main(int argc, char *argv[])
{   
    //init_test();
   // printf("done init\n");
    
    //test_fork();

    add_test();

    exit(1);
    
}