#include "/inc/base.inc"
#include "/inc/blueprint.inc"

#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, oldest, middle, current;
int request, checks, step;
mixed *blueprint_alias;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Generations: " + label + "\n");
}

void clean()
{
    blueprint_alias = 0;
    if (oldest) destruct(oldest);
    if (middle) destruct(middle);
    if (current) destruct(current);
    if (blueprint) destruct(blueprint);
    rm("generations_target.c");
    rm("migration-expect");
    rm("migration-observed");
}

void submit();
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        require(blueprint_outcome(report) == "completed",
                sprintf("generation preparation: %O", report["errors"]));
#ifdef __BLUEPRINT_UPDATE_TESTING__
        require(read_file("migration-observed") == (step ? "0\n" : "4\n"),
                "exact selected generations prepared");
#endif
        require(oldest.value() == 41 && middle.value() == 83 && current.value() == 127,
                "each old generation retains its final live value");
        require(oldest.shared_value()[0] == 41 && current.shared_value()[0] == 127,
                "retained shared declarations preserve clone-owned state");
        require(report["matched"] == 3 && report["already_current"] == (step ? 3 : 0)
                && report["updated"] == (step ? 0 : 3),
                "selection counts preserved");
        require(blueprint_alias[0][0] == 199, "shared added slot did not overwrite blueprint cell");
    }); publish);
    if (!err && !step++) { submit(); return; }
    if (!err) msg("BLUEPRINT_GENERATIONS: %d checks passed.\n", checks);
    clean();
    funcall(done, !!err);
}

void submit()
{
    rm("migration-observed"); rm("migration-expect");
    write_file("migration-expect", step ? "0 1\n" : "4 1\n");
    request = step ? update_blueprint(blueprint, ({oldest,middle,current}))
                   : update_blueprint("generations_target", ({oldest,middle,current}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    string functions = "int value(){return retained;} void seed(int n){retained=n;}\n";
    done = callback;
    clean();
    write_file("generations_target.c", "#pragma share_variables\nint retained; mixed *shared;\n"
        + functions + "void share(int n){shared=({n});} mixed *shared_value(){return shared;}\n");
    blueprint = load_object("generations_target"); oldest = clone_object(blueprint);
    oldest.seed(41); oldest.share(41);
    destruct(blueprint); rm("generations_target.c");
    write_file("generations_target.c", "#pragma share_variables\nint retained;\n" + functions);
    blueprint = load_object("generations_target"); middle = clone_object(blueprint); middle.seed(83);
    destruct(blueprint); rm("generations_target.c");
    write_file("generations_target.c", "#pragma share_variables\nmixed *shared=({19}); int retained;\n"
        + functions + "void share(int n){shared=({n});} mixed *shared_value(){return shared;} mixed *binding(){return ({&shared});}\n");
    blueprint = load_object("generations_target"); current = clone_object(blueprint);
    current.seed(127); current.share(127); blueprint.share(199);
    blueprint_alias = blueprint.binding();
    blueprint_alias[0] = ({199});
    require(blueprint.shared_value() == blueprint_alias[0], "fixture owns protected blueprint binding");
    submit();
}
#endif
