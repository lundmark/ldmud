#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/sys/object_info.h"

#if defined(__BLUEPRINT_UPDATE__) && __EFUN_DEFINED__(swap)
object source, target;
mixed *retained;
string source_name, target_name;
int request, phase, recovery, checks, hook_seen, gc_supported;
int source_version, expected_version, error_line;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Migration swap: " + label + "\n");
}

void runtime_error(string error, string program, string current, int line, mixed culprit, int caught)
{
    if (strstr(error, "swap line probe") >= 0) error_line = line;
}

void clean()
{
    foreach (string file: ({"before.gc", "pending.gc", "after.gc", "migration-swap-observed"}))
    {
        string evidence = read_file(file);
        if (evidence)
            write_file("swap-evidence.log", sprintf("PHASE %d RECOVERY %d %s\n%s\n", phase, recovery, file, evidence));
    }
    retained = 0;
    if (target) destruct(target);
    if (source) destruct(source);
    foreach (string file: ({"swap_target.c", "swap_hook.h", "before.gc", "pending.gc", "after.gc",
                           "migration-swap-fault", "migration-swap-observed"})) rm(file);
}

void release_trace()
{
    /* The DEBUG instruction ring owns object references. Overwrite its old
     * source entries before destruction cleanup tests sole program ownership.
     */
    for (int i = 0; i < 2048; i++) get_eval_cost();
}

void gc_seen(string file)
{
    string log = read_file(file);
    if (!log)
    {
        require(!gc_supported, "a supported collection must finish before the next state");
        return;
    }
    gc_supported = 1;
    write_file("swap-evidence.log", sprintf("GC PHASE %d RECOVERY %d %s\n%s\n", phase, recovery, file, log));
    require(!sizeof(regexp(explode(log, "\n"), "freeing.*block|tabled string.*was left unreferenced")),
            "legal swapped-state collection has no lost roots");
}

mixed include_file(string path, string from, int system)
{
    if (path == "swap_hook.h")
    {
        gc_seen("pending.gc");
        hook_seen++;
    }
    return 0;
}

void swapped(object ob, int program, int variables, string state)
{
    require(object_info(ob, OI_PROG_SWAPPED) == program
            && object_info(ob, OI_VAR_SWAPPED) == variables,
            sprintf("%s (%d/%d, image=%d)", state, object_info(ob, OI_PROG_SWAPPED),
                    object_info(ob, OI_VAR_SWAPPED), object_info(ob, OI_SWAP_NUM)));
}

void check_values(int installed)
{
    require(target && object_name(target) == target_name, "selected object retains its original identity");
    require(target.version() == (installed && phase != 2 ? expected_version : 1)
            && target.value() == 61, "selected behavior and retained value survive swap transitions");
    require(retained[0] == 61 && target.array() == retained, "external reference still owns the retained array");
    retained[0] = 62;
    require(target.array()[0] == 62, "retained array alias remains live");
    retained[0] = 61;
    if (source)
        require(object_name(source) == source_name && source.value() == 7
                && source.version() == (installed ? expected_version : source_version),
                "source identity, behavior and retained value survive swap transitions");
}

void next();
void after_gc()
{
    mixed err = catch(funcall(function void()
    {
        gc_seen("after.gc");
        if (phase == 2) swapped(source, 1, 1, "source-only commit remains program and variable swapped after GC");
        else swapped(target, 1, 1, "migrated sole-owner clone remains program and variable swapped after GC");
        check_values(1);
        require(update_blueprint_result(request)["status"] == "completed", "result remains readable after source destruction and GC");
        error_line = 0;
        if (phase == 2) catch(source.line_probe(); publish);
        else catch(target.line_probe(); publish);
        require(error_line > 0, "postcommit swapped program retains usable line numbers");
        clean();
        phase++;
        next();
    }); publish);
    if (err) { clean(); shutdown(1); }
}

void submit();
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        gc_seen("pending.gc");
        int fail = phase >= 3 && !recovery;
        require(report["status"] == (fail ? "failed" : "completed"),
                sprintf("phase %d expected injected failure or completed recovery, got %O", phase, report));
        if (fail)
        {
            require(!report["updated"] && !report["blueprint_updated"], "swap preparation failure has no partial commit");
            require(read_file("migration-swap-observed") != 0, "requested swap failure was actually reached");
            write_file("swap-evidence.log", read_file("migration-swap-observed"));
            check_values(0);
            rm("migration-swap-fault");
            recovery = 1;
            submit();
            return;
        }
        require(report["updated"] == (phase == 2 ? 0 : 1), "empty selection and migrated clone counts are exact");
        require(phase == 0 || hook_seen > 0, "compiler hook observes GC before candidate preparation");
        check_values(1);
        if (phase != 2)
            require(target.defaults()[0] == 5, "migrated clone has its complete new default");
#ifdef __BLUEPRINT_UPDATE_TESTING__
        int images, cold;
        string observed = read_file("migration-swap-observed");
        require(sscanf(observed ? observed : "", "%d %d", images, cold) == 2
                && images > 0 && cold > 0, "native publication verified old image invalidation and line preservation");
#endif
        if (phase == 2)
        {
            swap(source, 3);
            swapped(source, 1, 1, "source-only postcommit program and variables really swap");
        }
        else
        {
            destruct(source);
            source = 0;
            release_trace();
            /* Destruction cleanup drops the former source's program ref on
             * the following backend loop, before postcommit_swap executes.
             */
            call_out("postcommit_swap", 1);
            return;
        }
        garbage_collection("after.gc");
        call_out(#'after_gc, 1);
    }); publish);
    if (err) { clean(); shutdown(1); }
}

void postcommit_swap()
{
    mixed err = catch(funcall(function void()
    {
        require(object_info(target, OI_PROG_REFS) == 1,
                sprintf("retirement and source destruction release all extra program pins (%d)",
                        object_info(target, OI_PROG_REFS)));
        swap(target, 3);
        swapped(target, 1, 1, "postcommit clone program and variables really swap");
        garbage_collection("after.gc");
        call_out(#'after_gc, 1);
    }); publish);
    if (err) { clean(); shutdown(1); }
}

void submit()
{
    rm("pending.gc");
    rm("migration-swap-observed");
    request = phase == 0 ? update_blueprint(source, ({target}))
              : phase == 2 ? update_blueprint("swap_target", ({}))
                           : update_blueprint("swap_target", ({target}));
    require(update_blueprint_result(request)["status"] == "pending", "submission is deferred");
    swap(source, 3);
    swapped(source, 0, 1, "source pin prevents program swap but permits variable swap while pending");
    swap(target, 3);
    swapped(target, 1, 1, "older selected generation program and variables really swap while pending");
    if (phase >= 3 && !recovery)
        write_file("migration-swap-fault", sprintf("%d\n", phase - 2));
    garbage_collection("pending.gc");
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void before_gc()
{
    mixed err = catch(funcall(function void()
    {
        gc_seen("before.gc");
        swapped(source, 1, 1, "source remains fully swapped after pre-admission GC");
        swapped(target, 1, 1, "old clone remains fully swapped after pre-admission GC");
        submit();
    }); publish);
    if (err) { clean(); shutdown(1); }
}

void preswap()
{
    mixed err = catch(funcall(function void()
    {
        require(object_info(source, OI_PROG_REFS) == 1 && object_info(target, OI_PROG_REFS) == 1,
                sprintf("fixture has independent sole-owner source and old programs (%d/%d)",
                        object_info(source, OI_PROG_REFS), object_info(target, OI_PROG_REFS)));
        swap(source, 3); swap(target, 3);
        swapped(source, 1, 1, "source program and variables really swap before admission");
        swapped(target, 1, 1, "old target program and variables really swap before admission");
        garbage_collection("before.gc");
        call_out(#'before_gc, 1);
    }); publish);
    if (err) { clean(); shutdown(1); }
}

void next()
{
    if (phase == 6
#ifndef __BLUEPRINT_UPDATE_TESTING__
        || phase == 3
#endif
       )
    {
        msg("BLUEPRINT_SWAP: %d checks passed; GC supported=%d.\n", checks, gc_supported);
        shutdown(0); return;
    }
    recovery = hook_seen = 0;
    source_version = phase == 0 ? 2 : 1;
    expected_version = phase == 0 ? 2 : 3;
    string common = "void seed(int n){retained=n;payload=({n});} int value(){return retained;} mixed *array(){return payload;}\n"
                    "void line_probe(){raise_error(\"swap line probe\\n\");}\n";
    clean();
    write_file("swap_target.c", "#pragma init_variables\nint retained; int removed; mixed *payload;\nint version(){return 1;}\n" + common);
    source = load_object("swap_target"); target = clone_object(source);
    target.seed(61); retained = target.array(); target_name = object_name(target);
    destruct(source); rm("swap_target.c");
    write_file("swap_target.c", phase == 0
        ? "#pragma init_variables\nmixed *payload; int *fresh=({5}); int retained;\nint version(){return 2;} mixed *defaults(){return fresh;}\n" + common
        : "#pragma init_variables\nint retained; mixed *payload; int removed;\nint version(){return 1;}\n" + common);
    source = load_object("swap_target"); source.seed(7); source_name = object_name(source);
    if (phase != 0)
    {
        rm("swap_target.c");
        write_file("swap_hook.h", "\n");
        write_file("swap_target.c", "#pragma init_variables\n#include \"swap_hook.h\"\nmixed *payload; int *fresh=({5}); int retained;\n"
                  "int version(){return 3;} mixed *defaults(){return fresh;}\n" + common);
    }
    release_trace();
    call_out(#'preswap, 1);
}
#endif

string *epilog(int flag)
{
#if defined(__BLUEPRINT_UPDATE__) && __EFUN_DEFINED__(swap)
    if (catch(next(); publish)) { clean(); shutdown(1); }
#else
    msg("BLUEPRINT_SWAP: blueprint update or swap unavailable; skipped.\n");
    shutdown(0);
#endif
    return 0;
}
