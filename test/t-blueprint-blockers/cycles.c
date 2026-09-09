#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#ifdef __BLUEPRINT_UPDATE__
#define GC_LOG "blocker-cycles.gc.log"
closure done;
object blueprint, target, other;
mixed held;
int kind, stage, request, collections, checks;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Blocker cycles: " + why + "\n");
}

void clean()
{
    held = 0;
    if (target) destruct(target);
    if (other) destruct(other);
    if (blueprint) destruct(blueprint);
    rm("cycles_target.c");
    rm(GC_LOG);
}

void next();
void inspect()
{
    mapping report = update_blueprint_result(request);
    string expected = stage == 2 || kind == 6 || kind == 7 || kind == 8 ? "completed"
                                : kind == 3 || kind == 5 ? "LIVE_COROUTINE" : "LIVE_CLOSURE";
    mixed err = catch(
        require(blueprint_outcome(report) == expected,
                sprintf("kind %d stage %d expected %s, got %O", kind, stage, expected, report["errors"])),
        require(report["matched"] == 1, "counts survive GC and compatibility rejection"); publish);
    if (err) { clean(); funcall(done, 1); return; }
    msg("CYCLE_CASE %d.%d: %s\n", kind, stage, expected);
    stage++;
    if (stage == 3) { kind++; stage = 0; }
    next();
}

void collected(int retry)
{
    string log = read_file(GC_LOG);
    if (!log && !retry)
    {
        call_out(#'collected, __ALARM_TIME__, 1);
        return;
    }
    if (!log && !collections)
    {
        msg("BLUEPRINT_BLOCKER_CYCLES: skipped before allocation (no tracing GC).\n");
        clean(); funcall(done, 0); return;
    }
    if (catch(funcall(function void()
    {
        require(!!log && strstr(log, "--- Garbage Collection ---") >= 0,
                "actual tracing collection completed");
        /* Preserve the raw cycle sweep for native/Memcheck comparison. */
        write_file(sprintf("blocker-gc-%d.log", collections), log, 1);
        collections++;
        rm(GC_LOG);
        if (collections == 1) { next(); return; }
        request = update_blueprint("cycles_target", ({target}));
        call_out(#'inspect, __ALARM_TIME__ + 1);
    }); publish)) { clean(); funcall(done, 1); }
}

void collect()
{
    rm(GC_LOG);
    garbage_collection(GC_LOG);
    call_out(#'collected, __ALARM_TIME__, 0);
}

void next()
{
    if (kind == 9)
    {
        msg("BLUEPRINT_BLOCKER_CYCLES: %d checks, %d actual collections passed.\n", checks, collections);
        clean(); funcall(done, 0); return;
    }
    if (catch(funcall(function void()
    {
        if (!stage)
        {
            clean();
            write_file("cycles_target.c", "#include \"target.inc\"\n");
            blueprint = load_object("cycles_target");
            target = clone_object(blueprint); other = clone_object(blueprint);
            if (kind < 5) held = target.cycle(kind);
            if (kind == 5)
            {
                held = other.chain(target, other, 0);
                call_coroutine(held);
            }
            if (kind == 6)
            {
                held = ([bind_lambda(target.handle(5), other): 1]);
                destruct(other);
            }
            if (kind == 7)
            {
                held = target.handle(14);
                funcall(bind_lambda(held, other));
                destruct(other);
            }
            /* The unchanged unbound lambda calls its alien named closure
             * constant through FUNCALL; no target index is embedded.
             */
            if (kind == 8) held = unbound_lambda(0, ({target.handle(0)}));
        }
        if (stage == 2) held = 0;
        collect();
    }); publish)) { clean(); funcall(done, 1); }
}

void run(closure callback)
{
    done = callback;
    collect();
}
#endif
