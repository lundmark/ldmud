#define OWN_VALID_WRITE
#include "/inc/base.inc"
#include "/inc/gc.inc"

#if __EFUN_DEFINED__(async_write)
int returned, completed, submitted, errors;
int permission_nested, encoding_nested;
string *outputs = ({});

void check(string label, int condition)
{
    if (!condition)
    {
        errors++;
        msg("FAIL async_write: %s\n", label);
    }
}

void unexpected(int result)
{
    check("rejected/destroyed callback must never run", 0);
}

void finish(int result)
{
    foreach (string path: outputs) rm(path);
    shutdown(errors || result);
}

void timeout()
{
    msg("FAIL: asynchronous write timed out (%d/%d callbacks).\n", completed, submitted);
    shutdown(1);
}

void done(int index, int expected, int result)
{
    check("completion is deferred", returned);
    check("global FIFO callbacks", index == completed++);
    check("callback result", expected == result);
    if (completed == submitted)
    {
        check("append preserves order", read_file("ordered") == "first\nsecond\n");
        check("Unicode and embedded NUL bytes", read_bytes("latin") == b"\xe4\x00Z");
        check("stateful encoding flush", read_bytes("stateful") == b"\x1b$BF|\x1b(B");
        check("empty overwrite creates empty file", file_size("empty") == 0);
        check("permission recursion", permission_nested == 1);
        check("encoding recursion", encoding_nested == 1);
        check("source text captured", read_file("captured") == "before");
        check("destroyed callback still writes", read_file("destroyed") == "survives");
        check("recursive permission write", read_file("permission-child") == "child");
        check("recursive encoding write", read_file("encoding-child") == "child");
        remove_call_out(#'timeout);
        msg("Async writes: %d callbacks, %d errors.\n", completed, errors);
        start_gc(#'finish);
    }
}

void queue(string path, string text, int flag, int expected)
{
    int index = submitted++;
    outputs += ({ path });
    async_write(path, text, flag,
        function void(int result) { done(index, expected, result); });
}

mixed valid_write(string path, string uid, string func, object caller)
{
    if (path == "denied") return 0;
    if (path == "permission-parent" && func == "write_file" && !permission_nested++)
        queue("permission-child", "child", 1, 0);
    return path + "";
}

string encoding(string path)
{
    if (path == "encoding-parent" && !encoding_nested++)
        queue("encoding-child", "child", 1, 0);
    if (path == "latin") return "ISO-8859-1";
    if (path == "stateful") return "ISO-2022-JP";
    return "ASCII";
}

void raised(int result)
{
    check("exception callback success", result == 0);
    raise_error("Expected async callback exception.\n");
}

string *epilog(int flag)
{
    object dead;
    string source = "before";
    mixed error;
    call_out(#'timeout, 30);
    set_driver_hook(H_FILE_ENCODING, #'encoding);
    error = catch(async_write("denied", "x", 0, #'unexpected); nolog);
    check("permission rejection", !!error);
    error = catch(async_write("bad-flag", "x", 2, #'unexpected); nolog);
    check("flag rejection", !!error);
    error = catch(async_write("unrepresentable", "\u00e4", 0, #'unexpected); nolog);
    check("encoding rejection", !!error);
    error = catch(async_write("bad\0path", "x", 0, #'unexpected); nolog);
    check("NUL path rejection", !!error);
    error = catch(async_write("too-large", "x" * (16 * 1024 * 1024 + 1), 1, #'unexpected); nolog);
    check("bounded output rejection", !!error);
    check("rejections never touch destination", file_size("unrepresentable") == -1 && file_size("too-large") == -1);
    queue("ordered", "first\n", 1, 0);
    queue("ordered", "second\n", 0, 0);
    queue("latin", "\u00e4\0Z", 1, 0);
    queue("stateful", "\u65e5", 1, 0);
    queue("empty", "", 1, 0);
    queue("captured", source, 1, 0);
    source[0] = 'X';
    async_write("permission-parent", "parent", 1, #'raised);
    async_write("encoding-parent", "parent", 1, #'raised);
    outputs += ({ "permission-parent", "encoding-parent" });
    dead = clone_object("target");
    dead->submit();
    destruct(dead);
    outputs += ({ "destroyed" });
    for (int i = 0; i < 80; i++) queue("burst-" + i, "x", 1, 0);
    queue("missing/destination", "failure", 1, -1);
    returned = 1;
    garbage_collection();
    return 0;
}
#else
string *epilog(int flag)
{
    msg("Skipped: async file I/O is disabled.\n");
    shutdown(0);
    return 0;
}
#endif
