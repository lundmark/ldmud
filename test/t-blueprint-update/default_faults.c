#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/gc.inc"

#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, previous;
int checks, current, request, recovering;
mixed *cases;

void next_case();

void require(int condition, string label)
{
    checks++;
    if (!condition)
        raise_error("Default faults: " + label + "\n");
}

void clean()
{
    rm("defaults-fault");
    rm("defaults-compiler-fault");
    if (previous) destruct(previous);
    if (blueprint) destruct(blueprint);
    rm("defaults_target.c");
}

void collected(int failed)
{
    mixed err = catch(require(!failed, "partial defaults released after GC"); publish);
    if (!err)
        msg("BLUEPRINT_DEFAULT_FAULTS: %d checks passed.\n", checks);
    funcall(done, !!err);
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        string label = sprintf("point %d countdown %d mode %d recovery %d",
                               cases[current][0], cases[current][1], cases[current][2], recovering);
        require(report["status"] == "failed" && !report["updated"], label + ": no migration");
        require(report["errors"][0]["code"] == (recovering || cases[current][0] == 99
                   ? "IMPLEMENTATION_INCOMPLETE" : "VALIDATION_FAILED"), label + ": expected failure boundary");
        if (!recovering && cases[current][0] != 99)
            require(!sizeof(report["variable_changes"]), label + ": no partial terminal report");
        require(previous.retained_value() == 41 && blueprint.retained_value() == 41,
                label + ": live source and clone unchanged");
        rm("defaults-fault");
        rm("defaults-compiler-fault");
        if (!recovering)
        {
            recovering = 1;
            request = update_blueprint(cases[current][2] ? "defaults_target" : blueprint,
                        cases[current][2] == 2 ? ({}) : ({previous}));
            call_out(#'inspect, __ALARM_TIME__ + 1);
        }
        else
        {
            clean();
            recovering = 0;
            if (++current == sizeof(cases))
                start_gc(#'collected);
            else
                next_case();
        }
    }); publish);
    if (err)
    {
        clean();
        funcall(done, 1);
    }
}

void next_case()
{
    string common = "#pragma init_variables\nint retained = 41;\n"
                    "int retained_value() { return retained; }\n";
    clean();
    write_file("defaults_target.c", common);
    blueprint = load_object("defaults_target");
    previous = clone_object(blueprint);
    rm("defaults_target.c");
    write_file("defaults_target.c", common
        + "mixed prefix = ({1, ({2})});\n"
        + "mixed added = ([\"dup\":({([1:({2})])});([2:({3})]),"
        + "\"other\":({([3:({4})])});([4:({5})]),"
        + "\"dup\":({([5:({6})])});([6:({7})])]);\n"
        + "mixed tail = ({ b\"a\\x00z\", \"text\", ([({({\"key\"})}):({3});({4})]) });\n");
    if (!cases[current][2])
    {
        destruct(blueprint);
        blueprint = load_object("defaults_target");
    }
    write_file(cases[current][0] >= 35 && cases[current][0] <= 37 ? "defaults-compiler-fault" : "defaults-fault",
               sprintf("%d %d\n", cases[current][0], cases[current][1]));
    request = update_blueprint(cases[current][2] ? "defaults_target" : blueprint,
                              cases[current][2] == 2 ? ({}) : ({previous}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    done = callback;
    cases = ({});
    for (int mode = 0; mode < 3; mode++)
        foreach (int point : ({1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17}))
            cases += ({({point, point <= 5 ? 1
                                : point == 13 || point == 15 ? 4
                                : point == 14 ? 10 : point == 16 ? 20
                                : point == 17 ? 1 : 0, mode})});
    foreach (int point : ({35,36,37}))
        cases += ({({point, point == 37 ? 0 : 2, 1})});
    cases += ({({99, 0, 0})});
    if (file_size("defaults-fault-case") >= 0)
    {
        int selected = to_int(read_file("defaults-fault-case"));
        cases = ({cases[selected]});
    }
    next_case();
}
#endif
