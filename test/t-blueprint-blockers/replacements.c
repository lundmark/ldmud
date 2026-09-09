#include "/inc/base.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, target;
int phase, request, checks;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Replacement blockers: " + why + "\n");
}

void clean()
{
    rm("blockers-replacement-fault"); rm("blockers-replacement-hit");
    if (target) destruct(target);
    if (blueprint) destruct(blueprint);
}

void next();
void inspect()
{
    if (file_size("blockers-replacement-hit") <= 0
     && !function_exists("derived", target))
    {
        msg("BLUEPRINT_REPLACEMENT_BLOCKERS: allocation checkpoint unavailable in this build.\n");
        clean(); funcall(done, 0); return;
    }
    mixed err = catch(funcall(function void()
    {
        require(file_size("blockers-replacement-hit") > 0,
                "replacement variable allocation actually failed");
        require(target.derived() == 1 && target.read_value() == 41,
                "resource failure defers replacement with original program/state intact");
        if (!phase)
            require(!!catch(update_blueprint("replacement_target", ({target}))),
                    "admission rejects replacement still pending after a failed resource attempt");
        else
        {
            mapping report = update_blueprint_result(request);
            require(report["errors"][0]["code"] == "REPLACEMENT_PENDING",
                    "final validation rejects deferred replacement scheduled after admission");
            require(report["matched"] == 1 && !report["updated"],
                    "deferred conflict retains complete counts and no migration");
        }
    }); publish);
    if (err) { clean(); funcall(done, 1); return; }
    msg("REPLACEMENT_BLOCKER_CASE %d: resource-deferred pending entry rejected.\n", phase);
    phase++;
    clean();
    /* Existing replacement cleanup must retire the destroyed pending entry
     * before the next test creates a new blueprint in the same family.
     */
    call_out(#'next, __ALARM_TIME__ + 1);
}

void next()
{
    clean();
    if (phase == 2)
    {
        msg("BLUEPRINT_REPLACEMENT_BLOCKERS: %d checks passed.\n", checks);
        funcall(done, 0); return;
    }
    if (catch(funcall(function void()
    {
        blueprint = load_object("replacement_target");
        target = clone_object(blueprint);
        write_file("blockers-replacement-fault", object_name(target) + "\n");
        if (phase) request = update_blueprint("replacement_target", ({target}));
        target.replace_me();
        if (!phase)
            require(!!catch(update_blueprint("replacement_target", ({target}))),
                    "replacement queued before admission is rejected");
        call_out(#'inspect, 2 * __ALARM_TIME__ + 1);
    }); publish)) { clean(); funcall(done, 1); }
}

void run(closure callback) { done = callback; next(); }
#endif
