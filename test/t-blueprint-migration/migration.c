#include "/inc/base.inc"

#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, first, second;
mixed *protected_handles;
int request, checks;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Migration: " + label + "\n");
}

void clean()
{
    protected_handles = 0;
    if (first) { first.clear(); destruct(first); }
    if (second) { second.clear(); destruct(second); }
    if (blueprint) { blueprint.clear(); destruct(blueprint); }
    rm("migration_target.c");
    rm("migration-expect");
    rm("migration-observed");
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        require(report["errors"][0]["code"] == "IMPLEMENTATION_INCOMPLETE",
                sprintf("preparation completes: %O", report["errors"]));
        require(read_file("migration-observed") == "3\n", "three variable blocks were prepared");
        require(report["updated"] == 0, "preparation does not publish");
        require(blueprint.state() == 7 && first.state() == 41 && second.state() == 83,
                "distinct live values unchanged on rollback");
        require(first.array_value()[0] == 41 && second.array_value()[0] == 83,
                "clone aggregates unchanged");
        require(sizeof(report["variable_changes"][0]["matched"]) == 8,
                "retained declarations matched across reorder");
        require(sizeof(report["variable_changes"][0]["removed"]) == 1,
                "removed declaration described");
        msg("BLUEPRINT_MIGRATION: %d checks passed.\n", checks);
    }); publish);
    clean();
    funcall(done, !!err);
}

void run(closure callback)
{
    done = callback;
    clean();
    write_file("migration_target.c", "#pragma init_variables\n"
        "int retained; mixed *array; mapping map; mixed removed; mixed finished, dead, lightweight, stale, stale_cell;\n"
        "async int complete() { return 42; }\n"
        "void seed(int n) { retained=n; array=({n,0}); array[1]=array; map=([\"self\":0,\"array\":array]); map[\"self\"]=map; removed=array; finished=complete(); call_coroutine(finished); dead=clone_object(\"/empty\"); stale=symbol_function(\"get\",dead); stale_cell=stale; destruct(dead); lightweight=new_lwobject(\"/lightweight\"); }\n"
        "mixed *handles(){return ({&stale_cell});}\n"
        "void clear() { array[1]=0; map[\"self\"]=0; }\n"
        "int state() { return retained; } mixed *array_value() { return array; }\n");
    blueprint = load_object("migration_target");
    first = clone_object(blueprint);
    second = clone_object(blueprint);
    blueprint.seed(7); first.seed(41); second.seed(83);
    protected_handles = ({blueprint.handles(), first.handles(), second.handles()});
    rm("migration_target.c");
    write_file("migration_target.c", "#pragma init_variables\n"
        "mapping map; mixed *fresh=({({19}),([\"x\":({23})])}); mixed dead, finished, lightweight, stale, stale_cell; mixed *array; int retained; float zero;\n");
    write_file("migration-expect", "3 1\n");
    request = update_blueprint("migration_target", ({first, second}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
#endif
