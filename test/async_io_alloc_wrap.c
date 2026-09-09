/* Test-only linker wrappers. No allocation fault controls enter the driver
 * or its companion helper in an ordinary build. */
#include "driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xalloc.h"

#if !defined(USE_ASYNC_IO) || !defined(MALLOC_TRACE)
#error The allocation harness requires USE_ASYNC_IO and MALLOC_TRACE.
#endif

extern void *__real_xalloc_traced(size_t size MTRACE_DECL);
extern void *__real_rexalloc_traced(void *old, size_t size MTRACE_DECL);

/*-------------------------------------------------------------------------*/
static Bool
fail_allocation(const char *file, size_t size, Bool growth)
{
    static int selected;
    static unsigned int allocations;
    static Bool injected;
    const char *base = strrchr(file, '/');

    if (injected || strcmp(base ? base + 1 : file, "async_io.c"))
        return MY_FALSE;
    if (!selected)
    {
        const char *setting = getenv("ASYNC_ALLOC_FAIL");
        if (!setting || setting[0] < '1' || setting[0] > '3' || setting[1])
        {
            fprintf(stderr, "ASYNC_ALLOC_FAIL must be 1, 2, or 3.\n");
            exit(EXIT_FAILURE);
        }
        selected = setting[0] - '0';
    }
    allocations++;
    if ((selected == 1 && allocations == 1 && !growth)
     || (selected == 2 && allocations == 2 && !growth)
     || (selected == 3 && growth))
    {
        injected = MY_TRUE;
        fprintf(stderr, "ASYNC_ALLOC_FAIL case=%d kind=%s size=%zu\n",
                selected, growth ? "rexalloc" : "xalloc", size);
        return MY_TRUE;
    }
    return MY_FALSE;
}

void *
__wrap_xalloc_traced(size_t size MTRACE_DECL)
{
    if (fail_allocation(malloc_trace_file, size, MY_FALSE))
        return NULL;
    return __real_xalloc_traced(size MTRACE_PASS);
}

void *
__wrap_rexalloc_traced(void *old, size_t size MTRACE_DECL)
{
    if (fail_allocation(malloc_trace_file, size, MY_TRUE))
        return NULL;
    return __real_rexalloc_traced(old, size MTRACE_PASS);
}
