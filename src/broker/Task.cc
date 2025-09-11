#include <dlfcn.h>

#include <doca_error.h>
#include <doca_log.h>
#include <doca_pe.h>

#include "Task.h"

using namespace astraea;

DOCA_LOG_REGISTER(ASTRAEA : TASK);

/* Original function pointers */
doca_error_t (*original_doca_task_submit)(doca_task *task) =
    reinterpret_cast<doca_error_t (*)(doca_task *task)>(
        dlsym(RTLD_NEXT, "doca_task_submit"));
;
void (*original_doca_task_free)(doca_task *task) =
    reinterpret_cast<void (*)(doca_task *task)>(dlsym(RTLD_NEXT,
                                                      "doca_task_free"));
////////////////////////////////////////////////////////////////////////////////

doca_error_t doca_task_submit(doca_task *task) {
    auto myTask = reinterpret_cast<Task *>(task);
    return myTask->submit();
}

void doca_task_free(doca_task *task) {
    auto myTask = reinterpret_cast<Task *>(task);
    myTask->free();
    delete myTask;
}