#include "/inc/base.inc"
#include "/inc/blueprint.inc"

#ifndef PIPELINE_FIRST
#define PIPELINE_FIRST 0
#endif
#ifndef PIPELINE_STRIDE
#define PIPELINE_STRIDE 1
#endif

#ifdef __BLUEPRINT_UPDATE__
object source, first, second;
mixed *alias;
int request, previous_id, point, phase, checks, admissions, executions;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Pipeline faults: " + label + "\n");
}

void clean()
{
    alias = 0;
    if (first) destruct(first);
    if (second) destruct(second);
    if (source) destruct(source);
    rm("fault_target.c");
    rm("migration-pipeline");
    rm("migration-pipeline-observed");
    rm("migration-pipeline-collected");
}

void unchanged()
{
    require(first.version() == 1 && second.version() == 1
            && source.version() == (phase == 2 ? 2 : 1),
            "every original program survives failure");
    require(first.value() == 41 && second.value() == 83 && source.value() == 7,
            "every original variable block survives failure");
    require(!function_exists("defaults", first) && !function_exists("defaults", second),
            "no partial new default becomes visible");
    alias[0] = 42;
    require(first.value() == 42 && second.value() == 83, "retained protected cell still aliases its owner");
    alias[0] = 41;
}

void setup();
void submit();

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        require(report["status"] != "pending", "admitted attempt terminates at the backend boundary");
        if (report["status"] == "failed")
        {
            require(!report["updated"] && !report["blueprint_updated"], "failure never publishes a prefix");
            require(sizeof(report["errors"]) && report["completed_at"] > 0,
                    "admitted allocation failure keeps pollable terminal evidence");
#ifdef __BLUEPRINT_UPDATE_TESTING__
            require(read_file("migration-pipeline-collected") != 0,
                    "partial failure roots were traversed at the legal post-unwind checkpoint");
#endif
            report["errors"][0]["message"] = "changed";
            require(update_blueprint_result(request)["errors"][0]["message"] != "changed",
                    "failure report reads are independent and nonconsuming");
            unchanged();
            msg("BLUEPRINT_PIPELINE_POINT: %d %d execution\n", phase, point);
            executions++;
            point += PIPELINE_STRIDE;
            submit();
            return;
        }
#ifdef __BLUEPRINT_UPDATE_TESTING__
#if PIPELINE_FIRST == 0
        require(admissions > 0 && executions > 0,
                "contiguous sweep includes admission and execution failures before success");
#endif
#endif
        require(report["status"] == "completed" && report["updated"] == 2
                && report["blueprint_updated"] == (phase != 2), "recovery is a real complete installation");
        require(first.version() == 2 && second.version() == 2 && source.version() == 2,
                "successful recovery installs every selected original identity");
        require(first.value() == 41 && second.value() == 83 && source.value() == 7,
                "successful recovery preserves final live values");
        require(first.defaults()[0][0] == 19 && second.defaults()[1]["x"][0] == 23,
                "successful recovery materializes nested defaults");
        first.defaults()[0][0] = 99;
        require(second.defaults()[0][0] == 19 && source.defaults()[0][0] == 19,
                "new literal containers are independently owned");
        alias[0] = 42;
        require(first.value() == 42, "successful recovery preserves the retained alias");
        require(update_blueprint_result(request)["status"] == "completed",
                "success report remains nonconsuming after live mutation");
        msg("BLUEPRINT_PIPELINE_POINT: %d %d completed\n", phase, point);
        msg("BLUEPRINT_PIPELINE: mode %d, %d checks, %d admission failures, %d execution failures, success at %d.\n",
            phase, checks, admissions, executions, point);
        clean();
#ifdef PIPELINE_MODE
        shutdown(0);
#else
        if (++phase < 3) setup();
        else shutdown(0);
#endif
    }); publish);
    if (err) { clean(); shutdown(1); }
}

void submit()
{
    for (; point < 4000; point += PIPELINE_STRIDE)
    {
        mixed err;
        rm("migration-pipeline");
        rm("migration-pipeline-collected");
#ifdef __BLUEPRINT_UPDATE_TESTING__
        write_file("migration-pipeline", sprintf("%d\n", point));
#endif
        request = 0;
        err = catch(request = phase == 2 ? update_blueprint(source, ({first, second, first}))
                     : phase == 1 ? update_blueprint("fault_target")
                                  : update_blueprint("fault_target", ({first, second, first})); publish);
        if (err)
        {
            require(!request, "admission failure returns no ID");
            unchanged();
            msg("BLUEPRINT_PIPELINE_POINT: %d %d admission\n", phase, point);
            admissions++;
            continue;
        }
#ifdef __BLUEPRINT_UPDATE_TESTING__
        require(point > 0, "checkpoint zero must fail admission before an ID");
#endif
        require(request == previous_id + 1, "failed admissions consume no request identifiers");
        previous_id = request;
        require(update_blueprint_result(request)["status"] == "pending", "admission remains deferred");
        call_out(#'inspect, 1);
        return;
    }
    raise_error("Pipeline fault sweep did not reach success.\n");
}

void setup()
{
    string before = "#pragma init_variables\nint retained; int removed; mixed *array;\n"
        "void seed(int n){retained=n;array=({n});} int value(){return retained;} int version(){return 1;}\n"
        "mixed *aliases(){return ({&retained});}\n";
    string after = "#pragma init_variables\nmixed *array; mixed *fresh=({({19}),([\"x\":({23})])}); int retained;\n"
        "void seed(int n){retained=n;} int value(){return retained;} int version(){return 2;} mixed *defaults(){return fresh;}\n";
    clean();
    write_file("fault_target.c", before);
    source = load_object("fault_target");
    first = clone_object(source); second = clone_object(source);
    source.seed(7); first.seed(41); second.seed(83); alias = first.aliases();
    rm("fault_target.c"); write_file("fault_target.c", after);
    if (phase == 2)
    {
        destruct(source);
        source = load_object("fault_target"); source.seed(7);
    }
    admissions = executions = 0;
    point = PIPELINE_FIRST;
    submit();
}
#endif

string *epilog(int flag)
{
#ifdef PIPELINE_DETECT_ONLY
#ifdef __BLUEPRINT_UPDATE_TESTING__
    msg("BLUEPRINT_PIPELINE_CONFIG: testing\n");
#elif defined(__BLUEPRINT_UPDATE__)
    msg("BLUEPRINT_PIPELINE_CONFIG: ordinary\n");
#else
    msg("BLUEPRINT_PIPELINE_CONFIG: disabled\n");
#endif
    shutdown(0);
#else
#ifdef __BLUEPRINT_UPDATE__
#ifdef PIPELINE_MODE
    phase = PIPELINE_MODE;
#endif
    if (catch(setup(); publish)) { clean(); shutdown(1); }
#else
    msg("BLUEPRINT_PIPELINE: feature disabled.\n");
    shutdown(0);
#endif
#endif
    return 0;
}
