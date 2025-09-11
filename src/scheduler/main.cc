#include "../broker/shm.h"
#include "Scheduler.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    Scheduler scheduler;
    printf("Scheduler started\n");
    scheduler.schedule();
    printf("Scheduler stopped\n");
    return EXIT_SUCCESS;
}