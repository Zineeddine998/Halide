#include "HalideRuntime.h"

extern "C" {

WEAK int halide_default_do_task(void *user_context, halide_task_t f, int idx,
                                uint8_t *closure) {
    return f(user_context, idx, closure);
}

WEAK int halide_default_do_task_64(void *user_context, halide_task_64_t f, int64_t idx,
                                uint8_t *closure) {
    return f(user_context, idx, closure);
}

WEAK int halide_default_do_par_for(void *user_context, halide_task_t f,
                                   int min, int size, uint8_t *closure) {
    for (int x = min; x < min + size; x++) {
        int result = halide_do_task(user_context, f, x, closure);
        if (result) {
            return result;
        }
    }
    return 0;
}

WEAK int halide_default_do_par_for_64(void *user_context, halide_task_64_t f,
                                   int64_t min, int64_t size, uint8_t *closure) {
    for (int64_t x = min; x < min + size; x++) {
        int result = halide_do_task_64(user_context, f, x, closure);
        if (result) {
            return result;
        }
    }
    return 0;
}

}

namespace Halide { namespace Runtime { namespace Internal {

WEAK halide_do_task_t custom_do_task = halide_default_do_task;
WEAK halide_do_task_64_t custom_do_task_64 = halide_default_do_task_64;
WEAK halide_do_par_for_t custom_do_par_for = halide_default_do_par_for;
WEAK halide_do_par_for_64_t custom_do_par_for_64 = halide_default_do_par_for_64;

}}} // namespace Halide::Runtime::Internal

extern "C" {

WEAK halide_thread *halide_spawn_thread(void (*f)(void *), void *closure) {
    // We can't fake spawning a thread. Emit an error.
    halide_error(NULL, "halide_spawn_thread not implemented on this platform.");
    return NULL;
}

WEAK void halide_mutex_destroy(halide_mutex *mutex_arg) {
}

WEAK void halide_mutex_lock(halide_mutex *mutex) {
}

WEAK void halide_mutex_unlock(halide_mutex *mutex) {
}

WEAK void halide_shutdown_thread_pool() {
}

WEAK int halide_set_num_threads(int n) {
    if (n < 0) {
        halide_error(NULL, "halide_set_num_threads: must be >= 0.");
    }
    return 1;
}

WEAK halide_do_task_t halide_set_custom_do_task(halide_do_task_t f) {
    halide_do_task_t result = custom_do_task;
    custom_do_task = f;
    return result;
}

WEAK halide_do_task_64_t halide_set_custom_do_task_64(halide_do_task_64_t f) {
    halide_do_task_64_t result = custom_do_task_64;
    custom_do_task_64 = f;
    return result;
}

WEAK halide_do_par_for_t halide_set_custom_do_par_for(halide_do_par_for_t f) {
    halide_do_par_for_t result = custom_do_par_for;
    custom_do_par_for = f;
    return result;
}

WEAK halide_do_par_for_64_t halide_set_custom_do_par_for_64(halide_do_par_for_64_t f) {
    halide_do_par_for_64_t result = custom_do_par_for_64;
    custom_do_par_for_64 = f;
    return result;
}

WEAK int halide_do_task(void *user_context, halide_task_t f, int idx,
                        uint8_t *closure) {
    return (*custom_do_task)(user_context, f, idx, closure);
}

WEAK int halide_do_task_64(void *user_context, halide_task_64_t f, int64_t idx,
                        uint8_t *closure) {
    return (*custom_do_task_64)(user_context, f, idx, closure);
}

WEAK int halide_do_par_for(void *user_context, halide_task_t f,
                           int min, int size, uint8_t *closure) {
  return (*custom_do_par_for)(user_context, f, min, size, closure);
}

WEAK int halide_do_par_for_64(void *user_context, halide_task_64_t f,
                           int64_t min, int64_t size, uint8_t *closure) {
    return (*custom_do_par_for_64)(user_context, f, min, size, closure);
}

}  // extern "C"
