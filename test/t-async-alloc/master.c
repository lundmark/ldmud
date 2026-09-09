#include "/inc/base.inc"
#include "/inc/gc.inc"

#if defined(ASYNC_ALLOC_TEST) && __EFUN_DEFINED__(async_write)
int errors, callbacks, rejected_callbacks, returned;

void check(string label, int condition)
{
    if (!condition)
    {
        errors++;
        msg("FAIL async allocation: %s\n", label);
    }
}

void unexpected(int result)
{
    rejected_callbacks++;
    check("rejected request has no callback", 0);
}

void finish(int result)
{
    check("GC retained all owners", !result);
    check("exactly one successful callback", callbacks == 1);
    check("no rejected callback", !rejected_callbacks);
    check("rejected request never creates its file", file_size("rejected") == -1);
    if (!errors)
        write_file("completed", "PASS", 1);
    shutdown(!!errors);
}

void timeout()
{
    msg("FAIL async allocation: completion timed out.\n");
    shutdown(1);
}

void done(int result)
{
    callbacks++;
    check("completion is deferred", returned);
    check("recovery request succeeds", result == 0);
    check("recovery preserves all 12288 bytes", read_file("accepted") == "R" * 12288);
    check("rejected request has no file", file_size("rejected") == -1);
    remove_call_out(#'timeout);
    start_gc(#'finish);
}

string *epilog(int flag)
{
    mixed error;
    string expected = ASYNC_ALLOC_CASE == 1
        ? "*Out of memory for asynchronous file request.\n"
        : "*Out of memory for asynchronous file payload.\n";

    call_out(#'timeout, 30);
    error = catch(async_write("rejected", "F" * 12288, 1, #'unexpected); nolog);
    check("selected allocation fails synchronously", error == expected);
    check("preparation error leaves destination absent", file_size("rejected") == -1);
    check("preparation error does not invoke callback", !rejected_callbacks);
    async_write("accepted", "R" * 12288, 1, #'done);
    returned = 1;
    garbage_collection();
    return 0;
}
#else
string *epilog(int flag)
{
    msg("Skipped: requires the async allocation injection harness.\n");
    shutdown(0);
    return 0;
}
#endif
