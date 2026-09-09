#if __EFUN_DEFINED__(async_save_object)
int number;
string text;
mixed *shared;
mixed *alias;
mixed *cycle;
mapping wide;
mapping keys;
bytes binary;
float fraction;
closure saved_closure;
mixed *references;
mixed python_value;
nosave int skipped = 777;
static int hidden = 888;

int answer()
{
    return 42;
}

void setup(int rich)
{
    number = 42;
    text = "before \"quoted\"\nUnicode: \u00e5\n";
    shared = ({ 10, ({ 20, "nested" }) });
    alias = shared;
    cycle = allocate(1);
    cycle[0] = cycle;
    wide = ([ "key": 12; "value" ]);
    keys = ([ "member" ]);
    binary = rich ? b"\x00a\xff" : 0;
    fraction = 3.25;
    saved_closure = rich ? #'answer : 0;
    references = rich ? ({ &number, &(shared[0]) }) : 0;
    python_value = 0;
}

#if __EFUN_DEFINED__(make_save_probe)
void python_mode(int mode)
{
    python_value = make_save_probe(mode);
}

string legacy_value()
{
    return save_value(python_value);
}
#endif

void set_text_size(int size)
{
    text = "x" * size + "tail";
}

void set_number(int value)
{
    number = value;
}

string restore_legacy(string path)
{
    if (!restore_object(path))
        return "";
    return save_object();
}

void mutate()
{
    number = 99;
    text = "after";
    shared[0] = 999;
    shared[1][0] = 999;
    wide["key", 0] = 999;
    cycle[0] = 0;
    binary = b"changed";
    skipped = hidden = 999;
}

string legacy(int format)
{
    return save_object(format);
}

void submit(string path, closure callback, int format)
{
    async_save_object(path, callback, format);
}

void submit_default(string path, closure callback)
{
    async_save_object(path, callback);
}

int read_snapshot(string path, int rich)
{
    int ok = restore_object(path)
        && number == 42 && text == "before \"quoted\"\nUnicode: \u00e5\n"
        && shared[0] == 10 && shared[1][0] == 20 && shared[1][1] == "nested"
        && shared == alias && cycle[0] == cycle
        && widthof(wide) == 2 && wide["key", 0] == 12
        && wide["key", 1] == "value" && widthof(keys) == 0
        && member(keys, "member") && fraction == 3.25
        && skipped == 777 && hidden == 888
        && (!rich || (binary == b"\x00a\xff" && funcall(saved_closure) == 42));
    if (ok && rich)
    {
        references[0] = 73;
        references[1] = 74;
        ok = number == 73 && shared[0] == 74;
    }
    return ok;
}
#endif
