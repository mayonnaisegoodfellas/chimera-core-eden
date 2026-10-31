// Guest-only definitions of C library calls the sandbox does not provide.
// One static link: defining them here shadows musl's.
// SPDX-License-Identifier: GPL-3.0-or-later
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

extern "C" {

// Asked at start-up by libraries choosing between system calls: a fixed
// answer, the same on every host.
int uname(struct utsname *u) {
    if (!u) {
        errno = EFAULT;
        return -1;
    }
    memset(u, 0, sizeof *u);
    strcpy(u->sysname, "Linux");
    strcpy(u->nodename, "switch");
    strcpy(u->release, "5.15.0");
    strcpy(u->version, "#1");
    strcpy(u->machine, "x86_64");
    return 0;
}

// GNU's qsort_r over qsort: the comparison's argument rides in a thread
// local for the length of the call.
namespace {
struct SortContext {
    int (*compar)(const void *, const void *, void *);
    void *arg;
};
thread_local SortContext t_sort;
int sort_trampoline(const void *a, const void *b) {
    return t_sort.compar(a, b, t_sort.arg);
}
} // namespace

void qsort_r(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *, void *), void *arg) {
    const SortContext outer = t_sort;
    t_sort = { compar, arg };
    qsort(base, nmemb, size, sort_trampoline);
    t_sort = outer;
}

} // extern "C"
