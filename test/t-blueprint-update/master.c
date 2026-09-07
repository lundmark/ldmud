#pragma strong_types, save_types

#define OWN_PRIVILEGE_VIOLATION
#include "/inc/base.inc"
#include "/inc/gc.inc"

int checks;
int deny_update;
object first, second, replacement;
int pending_id;
int stage;

string *get_simul_efun()
{
    load_object("sefun");
    load_object("backup_sefun");
    return ({"/sefun", "/backup_sefun.c"});
}

int privilege_violation(string operation, mixed who, mixed source, mixed targets)
{
    if (operation != "update_blueprint")
        return 1;
    if (deny_update == 2)
        targets[0] = this_object();
    else if (deny_update == 3)
        destruct(who);
    else if (deny_update == 4)
        destruct(source);
    else if (deny_update == 5)
        destruct(targets[0]);
    return deny_update != 1;
}

#ifdef __BLUEPRINT_UPDATE__
#if !__EFUN_DEFINED__(update_blueprint) || !__EFUN_DEFINED__(update_blueprint_result)
#error Enabled blueprint update efuns are missing
#endif
#else
#if __EFUN_DEFINED__(update_blueprint) || __EFUN_DEFINED__(update_blueprint_result)
#error Disabled blueprint update efuns must be unavailable
#endif
#endif

void finish(int failed)
{
    rm("target.c");
    shutdown(failed);
}

void require(int condition, string description)
{
    checks++;
    if (!condition)
    {
        msg("FAILURE: %s\n", description);
        finish(1);
        raise_error(description + "\n");
    }
}

#ifdef __BLUEPRINT_UPDATE__
void advance();
void source_invalidated(int from_path);

void pending_gc_done(int failed)
{
    require(!failed, "pending request GC roots");
    advance();
}

void terminal_gc_done(int failed)
{
    mixed err = catch(
        require(!failed, "terminal report GC roots"),
        require(update_blueprint_result(pending_id)["status"] == "failed", "report survives GC"); publish);
    if (err)
        finish(1);
    else
    {
        msg("BLUEPRINT_UPDATE_ENABLED: %d checks passed.\n", checks);
        finish(0);
    }
}

void check_invalidated_source(int from_path)
{
    mixed err = catch(funcall(
        function void()
        {
            mapping report = update_blueprint_result(pending_id);
            require(report["status"] == "failed" && report["updated"] == 0
                    && sizeof(report["errors"])
                    && report["errors"][0]["code"] == "VALIDATION_FAILED",
                    from_path ? "deferred string source invalidation"
                              : "deferred object source invalidation");
            require(replacement.behavior_version() == 2
                    && first.behavior_version() == 1 && first.hp_value() == 41
                    && second.behavior_version() == 1 && second.hp_value() == 73,
                    "source revalidation preserves reloaded blueprint and old clones");
            if (!from_path)
                source_invalidated(1);
            else
                start_gc(#'terminal_gc_done);
        }); publish);
    if (err)
        finish(1);
}

void source_invalidated(int from_path)
{
    pending_id = from_path ? update_blueprint("/target.c", ({}))
                           : update_blueprint(replacement, ({}));
    require(update_blueprint_result(pending_id)["status"] == "pending",
            "empty-selection request is initially pending");
    destruct(replacement);
    replacement = load_object("target");
    call_out(#'check_invalidated_source, __ALARM_TIME__ + 1, from_path);
}

void lifecycle()
{
    mapping result;
    object *targets;

    require(!!catch(update_blueprint_result(0)), "invalid result id raises");
    require(!!catch(update_blueprint_result(2147483647)), "unknown result id raises");
    require(!!catch(update_blueprint("absent")), "string source never implicitly loads");
    require(!!catch(update_blueprint(first)), "clone source rejected");
    require(!!catch(update_blueprint(this_object())), "master source rejected");
    require(!!catch(update_blueprint(find_object("sefun"))), "primary sefun source rejected");
    require(!!catch(update_blueprint("backup_sefun")), "backup sefun source rejected");
    require(!!catch(update_blueprint(replacement, 1)), "nonzero selection rejected");
    require(!!catch(update_blueprint(replacement, ({load_object("v1")}))), "unrelated target rejected");
    require(!!catch(update_blueprint(replacement, ({0}))), "zero target rejected");
    deny_update = 1;
    require(!!catch(first.request_update(replacement)), "privilege denial raises");
    deny_update = 5;
    require(!!catch(first.request_explicit(replacement, ({clone_object(replacement)}))),
            "target destruction during privilege check");
    deny_update = 0;
    targets = ({first, first, second, replacement});
    deny_update = 2;
    pending_id = first.request_explicit(replacement, targets);
    deny_update = 0;
    require(!!catch(second.read_result(pending_id)), "unrelated owner cannot read report");
    require(pending_id > 0, "request id positive");
    require(pending_id == 1, "invalid admissions do not consume identifiers");
    targets[0] = this_object();
    result = update_blueprint_result(pending_id);
    require(result["id"] == pending_id && result["status"] == "pending"
            && result["selection"] == "explicit" && result["origin"] == "/target",
            "pending report identity and selection");
    require(result["completed_at"] == 0 && result["updated"] == 0
            && result["candidate_generation"] == 0 && !sizeof(result["errors"])
            && !sizeof(result["variable_changes"]), "pending report unknown values");
    result["status"] = "completed";
    result["errors"] += ({"mutated"});
    require(update_blueprint_result(pending_id)["status"] == "pending"
            && !sizeof(update_blueprint_result(pending_id)["errors"]), "report copy isolation");
    require(!!catch(update_blueprint("target")), "family admission lock");
    start_gc(#'pending_gc_done);
}

void advance()
{
    mixed err = catch(funcall(
        function void()
        {
            int previous_id = pending_id;
            mapping report = update_blueprint_result(pending_id);
            require(report["status"] == "failed" && report["updated"] == 0
                    && report["completed_at"] > 0 && sizeof(report["errors"])
                    && report["errors"][0]["code"] == "IMPLEMENTATION_INCOMPLETE",
                    "backend prototype fails without migrating");
            report["errors"][0]["code"] = "modified";
            require(update_blueprint_result(pending_id)["errors"][0]["code"]
                    == "IMPLEMENTATION_INCOMPLETE", "nested result copy isolation");
            require(first.behavior_version() == 1 && second.behavior_version() == 1
                    && first.hp_value() == 41 && second.hp_value() == 73,
                    "prototype preserves programs and state");
            stage++;
            if (stage == 1)
                pending_id = update_blueprint("/target.c");
            else if (stage == 2)
                pending_id = update_blueprint(replacement, 0);
            else if (stage == 3)
                pending_id = update_blueprint(replacement, ({}));
            else
            {
                object owner = clone_object(replacement);
                int canceled = owner.request_update(replacement);
                destruct(owner);
                require(!!catch(update_blueprint_result(canceled)), "owner destruction expires request");
                owner = clone_object(replacement);
                deny_update = 3;
                require(!!catch(owner.request_update(replacement)), "owner destruction during privilege check");
                require(!owner, "privilege callback destroyed owner");
                deny_update = 4;
                require(!!catch(first.request_update(load_object("v2"))), "source destruction during privilege check");
                deny_update = 0;
                require(checks >= 25, "enabled tests executed");
                source_invalidated(0);
                return;
            }
            require(pending_id == previous_id + 1, "request identifiers increase monotonically");
            require(update_blueprint_result(pending_id)["selection"]
                    == (stage == 3 ? "explicit" : "all"), "selection modes");
            call_out(#'advance, __ALARM_TIME__ + 1);
        }); publish);
    if (err)
        finish(1);
}
#endif

void run_test()
{
    object blueprint;

    msg("\nRunning blueprint update lifecycle tests:\n");
    call_out(#'finish, 20 * __ALARM_TIME__, 1);
    rm("target.c");
    copy_file("v1.c", "target.c");
    blueprint = load_object("target");
    first = clone_object(blueprint);
    second = clone_object(blueprint);
    first.set_hp(41);
    second.set_hp(73);
    require(first.hp_value() == 41 && second.hp_value() == 73,
            "clones retain distinct state");
    destruct(blueprint);
    rm("target.c");
    copy_file("v2.c", "target.c");
    replacement = load_object("target");
    require(replacement.behavior_version() == 2
            && replacement.charges_value() == 5, "reload selects version two");
    require(first.behavior_version() == 1 && second.behavior_version() == 1
            && first.hp_value() == 41 && second.hp_value() == 73,
            "ordinary reload preserves old clone programs and state");
    msg("Blueprint baseline: %d checks passed.\n", checks);
#ifdef __BLUEPRINT_UPDATE__
    lifecycle();
#else
    require(file_size("require-enabled") < 0, "blueprint update feature required");
    finish(0);
#endif
}

string *epilog(int eflag)
{
    mixed err = catch(run_test(); publish);
    if (err)
        finish(1);
    return 0;
}
