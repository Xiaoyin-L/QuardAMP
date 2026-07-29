#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void busy_work() {
    volatile int i;
    for (i = 0; i < 1000000; i++) { } // 稍微减少单次循环量
}

int main(int argc, char *argv[]) {
    int pid_low, pid_mid, pid_high;
    
    printf("Starting priority scheduler test...\n");

    // --- 创建 LOW 进程 ---
    pid_low = fork();
    if (pid_low < 0) {
        printf("fork failed\n");
        exit(1);
    }
    if (pid_low == 0) { // 子进程
        int mypid = getpid();
        printf("Child LOW created, pid: %d, setting prio to 1\n", mypid);
        
        // 关键：检查返回值
        if(setpriority(mypid, 1) < 0) {
            printf("Error: setpriority failed for LOW\n");
            exit(1);
        }
        
        int count = 0;
        uint64 start_time = uptime();
        // 跑 300 个 tick
        while (uptime() - start_time < 300) { 
            busy_work();
            count++;
        }
        printf(">>> LOW (prio 1) finished. Count: %d\n", count);
        exit(0);
    }

    // --- 创建 MID 进程 ---
    pid_mid = fork();
    if (pid_mid == 0) {
        int mypid = getpid();
        printf("Child MID created, pid: %d, setting prio to 5\n", mypid);
        
        if(setpriority(mypid, 5) < 0) {
            printf("Error: setpriority failed for MID\n");
            exit(1);
        }

        int count = 0;
        uint64 start_time = uptime();
        while (uptime() - start_time < 300) {
            busy_work();
            count++;
        }
        printf(">>> MID (prio 5) finished. Count: %d\n", count);
        exit(0);
    }

    // --- 创建 HIGH 进程 ---
    pid_high = fork();
    if (pid_high == 0) {
        int mypid = getpid();
        printf("Child HIGH created, pid: %d, setting prio to 10\n", mypid);
        
        if(setpriority(mypid, 10) < 0) {
            printf("Error: setpriority failed for HIGH\n");
            exit(1);
        }

        int count = 0;
        uint64 start_time = uptime();
        while (uptime() - start_time < 300) {
            busy_work();
            count++;
        }
        printf(">>> HIGH (prio 10) finished. Count: %d\n", count);
        exit(0);
    }

    // 父进程等待
    wait(0);
    wait(0);
    wait(0);
    
    printf("Test finished.\n");
    exit(0);
}