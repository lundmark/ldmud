#include "/inc/base.inc"

#if __EFUN_DEFINED__(async_write)
#ifndef EXPECT_EXIT
#define EXPECT_EXIT 0
#endif
int saved_value = 42;
nosave int completed;
nosave int notified;
nosave int errors;

void unexpected(int result)
{
    write_file("unexpected", "callback\n");
    shutdown(1);
}

void done(int result)
{
    if (result) errors++;
    if (!catch(async_write("recursive", "bad", 1, #'unexpected); nolog)) errors++;
    completed++;
#ifdef THROW_FIRST
    if (completed == 1) raise_error("Expected shutdown callback exception.\n");
#endif
    if (completed == 3)
    {
        if (notified != 1 || read_file("shutdown.txt") != "before\nnotify\n"
            || !restore_object("snapshot") || saved_value != 42) errors++;
#ifdef ASYNC_TEST_HARNESS
        write_file("completed", errors ? "FAIL" : "PASS", 1);
#else
        rm("shutdown.txt");
        rm("snapshot.o");
        msg("Shutdown drain: %d errors.\n", errors);
#endif
    if (errors) shutdown(1);
    }
}

void begin()
{
    async_write("shutdown.txt", "before\n", 1, #'done);
    async_save_object("snapshot", #'done);
    saved_value = 99;
    shutdown(EXPECT_EXIT);
}

void notify_shutdown()
{
    notified++;
    async_write("shutdown.txt", "notify\n", 0, #'done);
}

void flag(string arg)
{
#ifdef START_IN_FLAG
    begin();
#endif
}

string get_simul_efun()
{
#ifdef START_IN_SIMUL
    begin();
#endif
    return 0;
}

string *epilog(int flag)
{
    call_out(#'begin, 1);
    return 0;
}
#else
string *epilog(int flag)
{
    shutdown(0);
    return 0;
}
#endif
