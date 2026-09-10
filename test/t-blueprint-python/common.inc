#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"

#ifndef TARGET_PATH
#define TARGET_PATH "python_target"
#endif

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
object source, first, second;
closure done;
int request, checks;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Python handles: " + why + "\n");
}

#ifndef OWN_BODY
string body(int version, int change)
{
    return "#pragma strong_types, save_types, init_variables\n"
        + (version == 2 ? "int added = 912; int leading() { return -123; }\n" : "")
        + (version == 3 ? "int newer = 913, third = 914; int leading_newer() { return -124; } int preceding() { return -125; }\n" : "")
        + "public nosave int value = 41;\n"
        + (change == 2 ? "" : change == 3 ? "string doomed = \"different\";\n" : "int doomed = 17;\n")
        + (change == 4 ? "" : "mixed retired; void store(mixed arg) { retired = arg; }\n")
        + (change == 1 ? "" : "int discarded(int delta) { return 17 + delta; }\n")
        + sprintf("public nomask int read_value(int delta) { return value + delta + %d; }\n", version * 100)
        + "void set_value(int next) { value = next; }\n"
        + "closure variable_handle() { return #'value; }\n"
        + sprintf("int version() { return %d; }\n", version)
        + "closure generated() { return function int() { return 19; }; }\n"
        + (version == 3 ? "int added = 912; int leading() { return -123; }\n" : "");
}
#endif

void write_generation(int version, int change)
{
    rm(TARGET_PATH ".c");
    write_file(TARGET_PATH ".c", body(version, change));
}

void setup()
{
    write_generation(1, 0);
    source = load_object(TARGET_PATH);
    first = clone_object(source);
    second = clone_object(source);
    first.set_value(73);
    second.set_value(84);
}

void clean()
{
    python_handles_drop();
    if (first) destruct(first);
    if (second) destruct(second);
    if (source) destruct(source);
    rm(TARGET_PATH ".c");
}

void finish(int failed)
{
    clean();
    funcall(done, failed);
}
#endif
