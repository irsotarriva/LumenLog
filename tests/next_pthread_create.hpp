#ifndef LUMEN_TESTS_NEXT_PTHREAD_CREATE_HPP
#define LUMEN_TESTS_NEXT_PTHREAD_CREATE_HPP

#include <dlfcn.h>
#include <pthread.h>

// For the tests that replace pthread_create in their executable (Linux only)
// to make thread creation fail: the function their replacement calls when it
// lets a thread be created.
//
// That is a sanitizer's interceptor if there is one, otherwise the C
// library's. With GCC the sanitizer runtime is a shared library that the
// executable's definition comes before, so the next definition (RTLD_NEXT)
// is the interceptor. Clang links the runtime into the executable, where
// the replacement takes the interceptor's place; RTLD_NEXT is then the C
// library's, the sanitizer never sees the thread, and its pthread_join
// interceptor aborts ("Joining already joined thread"). The runtimes export
// the interceptor under this second name too, so it is looked up first.

namespace lumen::test {

using PthreadCreate = int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*);

[[nodiscard]] inline PthreadCreate next_pthread_create() {
    void* const interceptor = dlsym(RTLD_DEFAULT, "__interceptor_pthread_create");
    void* const next = interceptor != nullptr ? interceptor : dlsym(RTLD_NEXT, "pthread_create");
    // dlsym returns an untyped pointer.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<PthreadCreate>(next);
}

}  // namespace lumen::test

#endif
