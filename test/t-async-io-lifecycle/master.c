#include "/inc/base.inc"

#if __EFUN_DEFINED__(async_write) && defined(ASYNC_CASE)
nosave int count;
nosave int errors;
nosave int expected;
nosave int returned;

void check(int value, string name)
{
    if (!value)
    {
        errors++;
        msg("FAIL lifecycle: %s\n", name);
    }
}

void unexpected(int result)
{
    check(0, "rejected callback ran");
}

void timeout()
{
    msg("FAIL lifecycle: timeout\n");
    shutdown(1);
}

void tick()
{
    write_file("tick", "alive", 1);
}

void idle_probe()
{
    check(!!catch(async_write("unavailable", "x", 1, #'unexpected); nolog),
          "idle helper death closes admission");
    write_file("completed", errors ? "FAIL" : "PASS", 1);
    shutdown(errors != 0);
}

void complete(int index, int result)
{
    check(returned, "inline completion");
    check(index == count++, "FIFO callback order");
#if ASYNC_CASE == 2 || ASYNC_CASE == 5 || ASYNC_CASE == 6 || ASYNC_CASE == 8
    check(result == -1, "helper failure result");
    check(!!catch(async_write("unavailable", "x", 1, #'unexpected); nolog), "lost helper rejects admission");
#else
    check(result == 0, "successful result");
#endif
    if (count == expected)
    {
        write_file("completed", errors ? "FAIL" : "PASS", 1);
        remove_call_out(#'timeout);
        shutdown(errors != 0);
    }
}

string *epilog(int flag)
{
    mixed error;
    call_out(#'timeout, 20);
    call_out(#'tick, 1);
#if ASYNC_CASE == 7
    call_out(#'idle_probe, 5);
#elif ASYNC_CASE == 3
    expected = 1024;
    for (int i = 0; i < expected; i++)
        async_write("count", "", 0, function void(int result) { complete(i, result); });
    error = catch(async_write("overflow", "", 0, #'unexpected); nolog);
    check(!!error, "request count bound");
#elif ASYNC_CASE == 4
    expected = 4;
    for (int i = 0; i < expected; i++)
        async_write("bytes", "x" * (16 * 1024 * 1024), 0,
            function void(int result) { complete(i, result); });
    error = catch(async_write("overflow", "x", 0, #'unexpected); nolog);
    check(!!error, "total payload bound includes all requests");
#elif ASYNC_CASE == 5
    expected = 100;
    for (int i = 0; i < expected; i++)
        async_write("lost", "x", 0, function void(int result) { complete(i, result); });
    shutdown(0);
#elif ASYNC_CASE == 6 || ASYNC_CASE == 8
    expected = 1;
    async_save_object("uncertain", function void(int result) { complete(0, result); });
#if ASYNC_CASE == 8
    shutdown(0);
#endif
#else
    expected = 1;
    async_write("large", "x" * (2 * 1024 * 1024), 1,
        function void(int result) { complete(0, result); });
#endif
    returned = 1;
    garbage_collection();
    return 0;
}
#else
string *epilog(int flag)
{
    msg("Lifecycle scenarios run through run_async_io_lifecycle_test.sh.\n");
    shutdown(0);
    return 0;
}
#endif
