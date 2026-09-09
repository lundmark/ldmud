#include "/inc/base.inc"
#include "/inc/gc.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
#ifdef __PYTHON__
#define MAX_TRACE_DRAIN_STEPS 100000
object blueprint, target;
int phase, request, checks;
string expected;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Python blockers: " + why + "\n");
}

void clean()
{
    blocker_drop();
    if (target) destruct(target);
    if (blueprint) destruct(blueprint);
    rm("python_target.c");
}

void next();
void inspect()
{
    mapping report = update_blueprint_result(request);
    mixed err = catch(
        require(report["errors"][0]["code"] == expected,
                sprintf("case %d expected %s, got %O", phase, expected, report["errors"])),
        require(report["matched"] == 1 && !report["updated"], "selection count and no migration"); publish);
    if (err) { clean(); funcall(done, 1); return; }
    msg("PYTHON_BLOCKER_CASE %d: %s\n", phase, expected);
    phase++;
    next();
}

void collected(int failed)
{
    if (failed) { clean(); funcall(done, 1); return; }
    if (catch(funcall(function void()
    {
        if (phase / 2 == 16)
        {
            int remaining = blocker_binding_references();
            /* TRACE_CODE keeps counted LW objects in its instruction ring.
             * Executing this controller replaces those entries. Bound the
             * drain so an unexpected owner causes a failure, not a hang.
             */
            for (int step = 0; remaining > 1 && step < MAX_TRACE_DRAIN_STEPS; step++)
                remaining = blocker_binding_references();
            require(remaining == 1, "closure owns the final LW binding reference");
            blocker_rebind();
            require(blocker_finalizations() == 1,
                    "binding release finalizer observed coherent binding and released its handles");
        }
        request = update_blueprint("python_target", ({target}));
        if (phase % 2) blocker_drop();
        call_out(#'inspect, __ALARM_TIME__ + 1);
    }); publish)) { clean(); funcall(done, 1); }
}

void next()
{
    clean();
    if (phase == 34)
    {
        msg("BLUEPRINT_PYTHON_BLOCKERS: %d checks passed.\n", checks);
        funcall(done, 0); return;
    }
    if (catch(funcall(function void()
    {
        int kind = phase / 2;
        write_file("python_target.c", "#include \"target.inc\"\n");
        blueprint = load_object("python_target");
        target = clone_object(blueprint);
        blocker_hold(target, kind);
        expected = phase % 2 || kind == 11 || kind == 12 ? "IMPLEMENTATION_INCOMPLETE"
                 : kind < 6 || kind == 13 || kind == 14 ? "PYTHON_HANDLE"
                 : kind == 10 ? "LIVE_COROUTINE" : "LIVE_CLOSURE";
        start_gc(#'collected);
    }); publish)) { clean(); funcall(done, 1); }
}
#endif

void run(closure callback)
{
    done = callback;
#ifdef __PYTHON__
    next();
#else
    msg("BLUEPRINT_PYTHON_BLOCKERS: Python disabled.\n");
    funcall(done, 0);
#endif
}
#endif
