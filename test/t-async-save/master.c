#define OWN_VALID_WRITE
#include "/inc/base.inc"
#include "/inc/gc.inc"

#if __EFUN_DEFINED__(async_save_object)

nosave string *paths = ({});
nosave string *expected = ({});
nosave int *rich_values = ({});
nosave int completed;
nosave int returned;
nosave int errors;
nosave int nested_permissions;
nosave int nested_completed;
nosave int final_completed;
nosave string final_expected;

void nested_saved(int result)
{
    if (result)
        errors++;
    nested_completed++;
    rm("permission-inner.o");
}

void run_fifo_tests();

void check(string name, int ok)
{
    if (!ok)
    {
        errors++;
        msg("FAIL: %s\n", name);
    }
}

mixed valid_write(string path, string uid, string func, object ob)
{
    if (path == "denied")
        return 0;
    if (path == "destroyed")
    {
        destruct(ob);
        return 1;
    }
    if (path == "reentrant" && func == "save_object")
    {
        /* This hook runs before capture owns the serializer globals. */
        check("save_value in permission hook", stringp(save_value(({ 1, 2 }))));
        async_save_object("permission-inner", #'nested_saved);
        nested_permissions++;
    }
    return 1;
}

void timeout()
{
    msg("FAIL: asynchronous object save timed out.\n");
    shutdown(1);
}

void rejected_callback(int result)
{
    check("rejected request never invokes its callback", 0);
}

void saved(int index, int result)
{
    object restored = clone_object("fixture");
    check("callback runs after submission", returned);
    check("successful save callback", result == 0);
    check("FIFO save completion", index == completed++);
    check("same bytes as legacy serializer", read_file(paths[index] + ".o") == expected[index]);
    if (rich_values[index] < 0)
        check("large restored snapshot", restored->restore_legacy(paths[index]) == expected[index]);
    else
        check("restored captured values and aliases", restored->read_snapshot(paths[index], rich_values[index]));
    restored->mutate();
    destruct(restored);
    rm(paths[index] + ".o");
    if (completed == sizeof(paths))
    {
        check("permission hook reentrancy", nested_permissions == 1);
        check("nested accepted save completed", nested_completed == 1);
        run_fifo_tests();
    }
}

string *epilog(int flag)
{
    int *formats = ({ -1, 0, 1, 2, 3 });
    object source;
    mixed error;
    call_out(#'timeout, 20);
    foreach (int format: formats)
    {
        int index = sizeof(paths);
        string path = "format-" + (format + 1);
        int rich = format == -1 || format == 3;
        source = clone_object("fixture");
        source->setup(rich);
        paths += ({ path });
        expected += ({ source->legacy(format) });
        rich_values += ({ rich });
        source->submit(path + ".c", function void(int result) { saved(index, result); }, format);
        source->mutate();
        destruct(source);
    }
    source = clone_object("fixture");
    source->setup(1);
    paths += ({ "reentrant" });
    expected += ({ source->legacy(-1) });
    rich_values += ({ 1 });
    source->submit_default("reentrant", function void(int result) { saved(5, result); });
    error = catch(source->submit("denied", #'rejected_callback, -1));
    check("permission denial rejects synchronously", !!error);
    error = catch(source->submit("bad-format", #'rejected_callback, 4));
    check("invalid upper format rejects synchronously", !!error);
    error = catch(source->submit("bad-format", #'rejected_callback, -2));
    check("invalid lower format rejects synchronously", !!error);
    check("save_value remains usable after rejection", restore_value(save_value(({42})))[0] == 42);
    error = catch(source->submit("nul\0ignored", #'rejected_callback, -1));
    check("embedded filename NUL rejects synchronously", !!error);
    source->mutate();
    destruct(source);

    foreach (string path: ({ "large", "double.o", "unicode-\u00e5" }))
    {
        int index = sizeof(paths);
        source = clone_object("fixture");
        source->setup(0);
        source->set_text_size(20000);
        paths += ({ path });
        expected += ({ source->legacy(-1) });
        rich_values += ({ -1 });
        source->submit_default(path, function void(int result) { saved(index, result); });
        source->mutate();
        destruct(source);
    }

    source = clone_object("fixture");
    error = catch(source->submit("destroyed", #'rejected_callback, -1));
    check("caller destructed by permission hook rejects", !!error);
    {
        lwobject lightweight = new_lwobject("lightweight");
        error = catch(lightweight->submit(#'rejected_callback));
        check("lightweight caller rejects", !!error);
    }

    source = clone_object("fixture");
    source->setup(0);
    source->set_text_size(17 * 1024 * 1024);
    error = catch(source->submit("oversized", #'rejected_callback, -1));
    check("oversized capture rejects synchronously", !!error);
    source->set_text_size(10);
    check("serializer recovers after output limit", stringp(source->legacy(-1)));
    source->mutate();
    destruct(source);
#if __EFUN_DEFINED__(make_save_probe)
    source = clone_object("fixture");
    source->setup(0);
    foreach (int mode: ({ 1, 2, 3, 4 }))
    {
        string reason = mode == 1 ? "injected Python save failure" : "Nested save serialization";
        source->python_mode(mode);
        error = catch(source->submit("python-failed", #'rejected_callback, -1));
        check("Python errors and nested saves reject async capture", stringp(error) && strstr(error, reason) >= 0);
        error = catch(source->legacy(-1));
        check("Python errors and nested saves reject legacy object save", stringp(error) && strstr(error, reason) >= 0);
        error = catch(source->legacy_value());
        check("Python errors and nested saves reject legacy value save", stringp(error) && strstr(error, reason) >= 0);
        source->mutate();
        source->setup(0);
        check("serializer recovers after Python unwind", stringp(source->legacy(-1)));
    }
    {
        int index = sizeof(paths);
        source->python_mode(0);
        paths += ({ "python-success" });
        expected += ({ source->legacy(-1) });
        rich_values += ({ -1 });
        source->submit_default("python-success", function void(int result) { saved(index, result); });
    }
    source->mutate();
    destruct(source);
    source = clone_object("fixture");
    source->setup(0);
    source->mutate();
    source->python_mode(5);
    error = catch(source->submit("python-destructed", #'rejected_callback, -1));
    check("Python hook destruction rejects before publication", stringp(error)
        && strstr(error, "Object destructed during save serialization") >= 0);
    check("serializer recovers after hook destruction", restore_value(save_value(42)) == 42);
#endif
    returned = 1;
    garbage_collection();
    return 0;
}

void fifo_done(int index, int result)
{
    check("shared write/save queue FIFO", final_completed++ == index);
    check("FIFO request result", result == (index == 4 ? -1 : 0));
    if (index == 4)
    {
        check("latest save followed by append is final", read_file("fifo.o") == final_expected + "\n");
        check("failed rename preserves prior destination", read_file("blocked.o/old") == "old");
        rm("fifo.o");
        rm("blocked.o/old");
        rmdir("blocked.o");
        remove_call_out(#'timeout);
        msg("Async object saves: %d capture and %d FIFO callbacks, %d errors.\n",
            completed, final_completed, errors);
        start_gc(function void(int result) { shutdown(errors || result); });
    }
}

void run_fifo_tests()
{
    object source = clone_object("fixture");
    source->setup(0);
    source->submit_default("fifo", function void(int result) { fifo_done(0, result); });
    async_write("fifo.o", "intermediate", 1, function void(int result) { fifo_done(1, result); });
    source->set_number(123);
    final_expected = source->legacy(-1);
    source->submit_default("fifo", function void(int result) { fifo_done(2, result); });
    async_write("fifo.o", "\n", 0, function void(int result) { fifo_done(3, result); });
    mkdir("blocked.o");
    write_file("blocked.o/old", "old", 1);
    source->submit_default("blocked", function void(int result) { fifo_done(4, result); });
    source->mutate();
    destruct(source);
}

#else
mixed valid_write(string path, string uid, string func, object ob)
{
    return 1;
}

string *epilog(int flag)
{
    msg("Skipping async object saves: async_save_object is not enabled.\n");
    shutdown(0);
    return 0;
}
#endif
