#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/deep_eq.inc"
#if defined(__BLUEPRINT_UPDATE__)
object source;
object *targets, *sides;
closure *held;
closure done;
int phase, request, checks;
void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Advanced reports: " + why + "\n");
}
void cleanup()
{
    held = 0;
    foreach (object ob : targets + sides + ({source})) if (ob) destruct(ob);
    rm("advanced_target.c"); rm("advanced_side.c"); rm("requests-limits");
}
void finish(int failed) { cleanup(); funcall(done, failed); }
void next_case();
void inspect()
{
    mapping result = update_blueprint_result(request);
    string code = phase == 1 ? "UNSUPPORTED_OBJECT" : phase == 2 ? "LIVE_CLOSURE" : "SCHEMA_INCOMPATIBLE";
    if (catch(
        require(result["status"] == "failed" && blueprint_outcome(result) == code, sprintf("primary %s: %O", code, result["errors"])),
        require(result["matched"] == sizeof(targets) && !result["updated"] && !result["blueprint_updated"], "complete counts and rollback"),
        require(targets[0].read_value() == 41, "first instance stays unchanged"); publish))
    { finish(1); return; }
#ifdef __BLUEPRINT_UPDATE_TESTING__
    if (phase == 3)
    {
        if (catch(require(result["errors"][0]["incomplete"] == 1
                       && result["errors"][0]["incomplete_code"] == "RUNTIME_DEPENDENCY_LIMIT", "later work exhaustion preserves primary schema reason"); publish))
        { finish(1); return; }
    }
    else
#endif
    {
        foreach (object ob : targets)
        {
            mapping *rows = filter(result["errors"], (: $1["object"] == object_name(ob) && $1["code"] == code :));
            if (catch(require(sizeof(rows) == 1, "every matching instance represented exactly once"); publish))
            { finish(1); return; }
            if ((phase == 0 || phase == 3) && catch(require(sizeof(filter(result["errors"], (: $1["object"] == object_name(ob) && $1["code"] == "LIVE_CLOSURE" :))) == 1, "schema rejection still discovers runtime blocker"); publish))
            { finish(1); return; }
        }
    }
    cleanup();
    if (++phase == 4)
    {
        msg("BLUEPRINT_REPORTS_ADVANCED: %d checks, schema, unsupported instances, cohort size and limits passed.\n", checks);
        funcall(done, 0);
    }
    else next_case();
}
void next_case()
{
    int count = phase == 2 ? 140 : 2;
    sides = ({}); targets = ({}); held = ({});
    write_file("advanced_target.c", "#pragma strong_types\nint value = 41;\n"
        "mixed read_value() { return value; }\n"
        "void start_shadow(object ob) { shadow(ob); }\n"
        "closure handle() { return function mixed() { return value; }; }\n", 1);
    source = load_object("advanced_target");
    for (int i = 0; i < count; i++) targets += ({clone_object(source)});
    if (phase != 1)
        foreach (object ob : targets)
            for (int i = 0; i < (phase == 3 ? 3000 : 1); i++) held += ({ob.handle()});
    write_file("advanced_target.c", (phase == 1 || phase == 2 ? "int value = 42;\n" : "string value = \"incompatible\";\n")
        + "mixed read_value() { return value; }\nvoid start_shadow(object ob) { shadow(ob); }\n"
          "closure handle() { return function mixed() { return value; }; }\n", 1);
#ifdef __BLUEPRINT_UPDATE_TESTING__
    if (phase == 3) write_file("requests-limits", "10000 1000000 4000\n", 1);
#endif
    request = update_blueprint("advanced_target", targets);
    rm("requests-limits");
    if (phase == 1)
    {
        write_file("advanced_side.c", "int side;\n", 1);
        foreach (object ob : targets)
        {
            object side = clone_object("advanced_side");
            sides += ({side}); ob.start_shadow(side);
        }
    }
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
void run(closure callback)
{
    done = callback;
    if (catch(next_case(); publish)) finish(1);
}
#endif
