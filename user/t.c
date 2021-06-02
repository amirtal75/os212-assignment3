#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"



void add_test()
{
    int pid = getpid();
    procdump(pid,pid,0);
    char* buffer = malloc (4096*15);
    for (int i = 0; i < 10; i++)
    {
        *(buffer + (4096*i)) = (49 + i);
        if (i %2 == 0)
        {
            *(buffer + (4096*(i+5))) = (48 + i);
        }
        printf("\n\nstart test: %d\n\n",i+1);
        procdump(pid,pid,0);
        printf("\nend test: %d\n\n\n",i+1);
    }
    
}
// tests fork, copy metadata, copy file, init_metadata
void 
test_fork()
{
    int pid;
    int parentpid = getpid();
    add_test();
    if ((pid = fork()) > 0)
    {
        printf("in fork\n\n");
        procdump(parentpid,pid,0);
    }
    else if (pid == 0)
    {
        
    }
    else
    {
        printf("fork failed\n");
    }   
}
void init_test()
{
    printf("before\n");
    int pid = getpid();
    printf("parent pid: %d\n", pid);
    procdump(pid,pid,0);
}

void swapin_test()
{
    printf("before\n");
    int pid = getpid();
    printf("parent pid: %d\n", pid);
    int test_case = 1;
    procdump(pid,pid, test_case);
}

void fifo_test()
{
    char* pages[16];
    for (int i = 0; i < 16; i++)
    {
        pages[i] = (char*)sbrk(4096);
    }
    printf("created 16 pages, the first addr is : %x\n",pages[0]);


}
int
main(int argc, char *argv[])
{   
    // init_test();
    // printf("done init\n");
    
    // test_fork();

    // add_test();
    fifo_test();

    exit(1);
    
}