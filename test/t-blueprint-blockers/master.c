#include "/inc/base.inc"
#include "/inc/blueprint.inc"

#ifdef __BLUEPRINT_UPDATE__
object blueprint, target, other;
mixed held;
int request, step, checks;
string expected, label;

void require(int ok, string description)
{
    checks++;
    if (!ok) raise_error("Blockers: " + description + "\n");
}

void clean()
{
    held = 0;
    if (target) destruct(target);
    if (other) destruct(other);
    if (blueprint) destruct(blueprint);
    rm("target.c");
}

void next();
void inspect()
{
    mapping report = update_blueprint_result(request);
    mixed err = catch(
        require(report["status"] == (expected == "completed" ? "completed" : "failed"), label + " has its expected outcome"),
        require(blueprint_outcome(report) == expected,
                sprintf("%s: expected %s, got %O", label, expected, report["errors"])),
        require(target.read_value() == 41, label + " preserves state"); publish);
    if (err) { clean(); shutdown(1); return; }
    msg("BLOCKER_CASE %d %s: %s\n", step, label, expected);
    step++;
    next();
}

void next()
{
    mixed err;
    clean();
    if (step == 40)
    {
        msg("BLUEPRINT_BLOCKERS: %d checks passed.\n", checks);
        load_object("bindings").run(function void(int failed)
        {
            if (failed) shutdown(1);
            else load_object("replacements").run(function void(int failed)
            {
                if (failed) shutdown(1);
                else load_object("cycles").run(function void(int failed)
                {
                    if (failed) shutdown(1);
                    else load_object("python").run(#'shutdown);
                });
            });
        });
        return;
    }
    err = catch(funcall(function void()
    {
        write_file("target.c", "#include \"target.inc\"\n");
        blueprint = load_object("target");
        target = clone_object(blueprint);
        other = clone_object(blueprint);
        expected = "completed";
        if (step < 30)
        {
            int kind = step / 2;
            label = sprintf("closure %d %s", kind, step % 2 ? "released after admission" : "retained");
            held = target.handle(kind);
            if (!(step % 2) && kind < 13
             && member(({0,1,6,7,8,10}),kind) < 0) expected = "LIVE_CLOSURE";
        }
        else if (step < 39)
        {
            label = sprintf("coroutine %d", step - 30);
            held = target.sleeper(step == 34);
            if (step == 30 || step == 31 || step == 35 || step == 36) expected = "LIVE_COROUTINE";
            if (step != 30 && step != 35) call_coroutine(held);
            if (step == 33) call_coroutine(held);
            if (step == 34) catch(call_coroutine(held));
            if (step == 35)
            {
                held = other.waiter(held);
                call_coroutine(held);
            }
            if (step >= 36)
            {
                held = other.chain(target, other, step == 38);
                call_coroutine(held);
                if (step == 37) call_coroutine(held);
                if (step == 38) catch(call_coroutine(held));
            }
        }
        else
        {
            label = "named closure created after admission";
        }
        request = update_blueprint("target", ({target}));
        if (step == 39) held = target.handle(0);
        if ((step < 30 && step % 2) || step == 32) held = 0;
        call_out(#'inspect, __ALARM_TIME__ + 1);
    }); publish);
    if (err) { clean(); shutdown(1); }
}
#endif

void run_test()
{
#ifdef __BLUEPRINT_UPDATE__
    next();
#else
    msg("BLUEPRINT_BLOCKERS: feature disabled.\n");
    shutdown(0);
#endif
}

string *epilog(int eflag) { run_test(); return 0; }
