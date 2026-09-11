#include "common.inc"

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
void inspect()
{
    mapping report = update_blueprint_result(request);
    rm("requests-limits");
    if (catch(
        require(blueprint_outcome(report) == "RUNTIME_DEPENDENCY_LIMIT",
                sprintf("bounded Python inventory expected RUNTIME_DEPENDENCY_LIMIT, got %s", blueprint_outcome(report))),
        require(!report["updated"] && !report["blueprint_updated"], "bounded scan publishes no prefix"),
        require(first.version() == 1 && second.version() == 1 && source.version() == 1,
                "bounded scan preserves every program"),
        require(python_handles_check(first, 1), "bounded scan preserves retained Python key state"); publish))
    { finish(1); return; }
    msg("BLUEPRINT_PYTHON_LIMIT: %d checks, weak inventory exhaustion leaves cohort unchanged.\n", checks);
    finish(0);
}

void run(closure callback)
{
    done = callback;
#ifdef __BLUEPRINT_UPDATE_TESTING__
    if (catch(
        setup(), require(python_handles_hold(first, second, 0), "keys before bounded scan"),
        python_handles_inventory(first),
        rm("requests-limits"), write_file("requests-limits", "10000 1000000 4000\n"),
        write_generation(2, 0), request = update_blueprint(TARGET_PATH, ({first, second})),
        call_out(#'inspect, __ALARM_TIME__ + 1); publish))
    { rm("requests-limits"); finish(1); }
#else
    msg("BLUEPRINT_PYTHON_LIMIT: test limits disabled.\n");
    funcall(done, 0);
#endif
}
#endif
