#include "/inc/base.inc"
#include "/inc/blueprint.inc"

#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, first, second;
mixed *alias;
int request, checks, step;
int *points = ({0,1,2,3,4,5,7,10,13,-1});

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Migration rollback: " + label + "\n");
}

void clean()
{
    alias = 0;
    if (first) destruct(first);
    if (second) destruct(second);
    if (blueprint) destruct(blueprint);
    rm("fault_target.c");
    rm("migration-fault");
    rm("migration-released");
}

void submit();
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        string expected = points[step] < 0 ? "completed" : "PREPARATION_FAILED";
        require(blueprint_outcome(report) == expected,
                sprintf("point %d expected %s, got %O", points[step], expected, report["errors"]));
        require(report["matched"] == 2 && report["updated"] == (points[step] < 0 ? 2 : 0),
                "complete counts and publication only on recovery");
        if (points[step] >= 0)
            require(find_object("master").released_before_error(), "rollback precedes runtime_error hook");
        require(blueprint.value() == 7 && first.value() == 41 && second.value() == 83,
                "all live values unchanged after partial staging");
        alias[0] = 42;
        require(first.value() == 42 && second.value() == 83, "protected cell remains valid after rollback");
        alias[0] = 41;
    }); publish);
    if (!err && ++step < sizeof(points)) { submit(); return; }
    if (!err) msg("BLUEPRINT_MIGRATION_ROLLBACK: %d checks passed.\n", checks);
    clean();
    funcall(done, !!err);
}

void submit()
{
    rm("migration-fault");
    rm("migration-released");
    if (points[step] >= 0) write_file("migration-fault", sprintf("%d\n", points[step]));
    request = update_blueprint("fault_target", ({first,second}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    done = callback;
    clean();
    write_file("fault_target.c", "#pragma init_variables\nint retained; mixed *array;\n"
        "void seed(int n){retained=n;array=({n});} int value(){return retained;} mixed *aliases(){return ({&retained});}\n");
    blueprint=load_object("fault_target"); first=clone_object(blueprint); second=clone_object(blueprint);
    blueprint.seed(7); first.seed(41); second.seed(83); alias=first.aliases();
    alias[0] = 42; require(first.value() == 42, "fixture owns a real protected variable alias"); alias[0] = 41;
    rm("fault_target.c");
    write_file("fault_target.c", "#pragma init_variables\nmixed *array; mixed *fresh=({({19}),([\"x\":({23})])}); int retained;\n"
        "int value(){return retained;}\n");
#ifndef __BLUEPRINT_UPDATE_TESTING__
    points = ({-1});
#endif
    submit();
}
#endif
