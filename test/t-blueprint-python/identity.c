#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
object source, target;
closure done;
int request, checks;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Python identity: " + why + "\n");
}

void finish(int failed)
{
    python_handles_drop();
    if (target) destruct(target);
    if (source) destruct(source);
    rm("identity_target.c");
    funcall(done, failed);
}

void destroyed(int failed)
{
    if (catch(
        require(!failed, "mixed named and generated wrappers survive target GC"),
        require(python_handles_identity_destroyed(), "comparisons do not dereference destroyed bindings"); publish))
    { finish(1); return; }
    msg("BLUEPRINT_PYTHON_IDENTITY: %d checks, colliding named/generated keys and ordering passed.\n", checks);
    finish(0);
}

void inspect()
{
    mapping result = update_blueprint_result(request);
    if (catch(
        require(result["status"] == "completed", sprintf("migration before collision: %O", result["errors"])),
        require(result["updated"] == 1 && result["blueprint_updated"], "entire cohort installed"),
        require(target.alpha() == 141, "retained state and changed code"),
        require(python_handles_identity_check(target), "distinct keys despite coinciding rank and slot"),
        destruct(target), destruct(source),
        start_gc(#'destroyed); publish)) finish(1);
}

void run(closure callback)
{
    done = callback;
    if (catch(
        write_file("identity_target.c", "#pragma strong_types, save_types\n"
            "int value = 41;\n"
            "int prefix() { return 11; }\n"
            "int alpha() { return value; }\n"
            "closure make_inline() { return 0; }\n"),
        source = load_object("identity_target"), target = clone_object(source),
        require(python_handles_identity_hold(target), "named dictionary and set keys before migration"),
        rm("identity_target.c"),
        write_file("identity_target.c", "#pragma strong_types, save_types\n"
            "int value = 41;\n"
            "closure make_inline() { return function int() { return 19; }; }\n"
            "int prefix() { return 11; }\n"
            "int alpha() { return value + 100; }\n"),
        request = update_blueprint("identity_target", ({target})),
        call_out(#'inspect, __ALARM_TIME__ + 1); publish)) finish(1);
}
#endif
