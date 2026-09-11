#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"
#include "/sys/configuration.h"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint;
object *clones = ({});
int request, checks, phase;
int nested, saw_terminal, hook_calls;
int before_replacement;
object side;

void require(int condition, string description);

void heart_beat()
{
    if (nested && update_blueprint_result(request)["status"] == "completed")
    {
        saw_terminal = update_blueprint_result(nested)["status"] == "pending";
        configure_object(this_object(), OC_HEART_BEAT, 0);
    }
}

void compiler_hook()
{
    mapping pending = update_blueprint_result(request);
    hook_calls++;
    require(pending["status"] == "pending" && !pending["matched"]
            && !pending["candidate_generation"] && !sizeof(pending["errors"]),
            "in-flight polling exposes only pending unknowns");
    if (phase >= 20)
    {
        require(update_blueprint_result(nested)["status"] == "pending",
                "earlier batch member is active while later source request remains pending");
        clones[0].request_replace();
        destruct(clones[2]);
        require(clones[0].value() == 41 && blueprint.value() == 41,
                "queuing replacement leaves selected clone and source values intact during the hook");
        return;
    }
    if (phase == 16)
    {
        clones[0].request_replace();
        return;
    }
    if (phase >= 11)
    {
        destruct(clones[0]);
        return;
    }
    clones += ({clone_object(blueprint)});
    if (phase >= 8) clones[<1].start_shadow(side);
    if (phase == 7) nested = update_blueprint(side, ({}));
}

void require(int condition, string description)
{
    checks++;
    if (!condition) raise_error("Requests: " + description + "\n");
}

void clean()
{
    foreach (object ob: clones) if (ob) destruct(ob);
    clones = ({});
    if (blueprint) destruct(blueprint);
    if (side) destruct(side);
    rm("requests_target.c");
    rm("requests_side.c"); rm("requests_hook.h");
    rm("requests-default-once"); rm("requests-limits");
}

void source(string code)
{
    if (blueprint) destruct(blueprint);
    rm("requests_target.c");
    write_file("requests_target.c", code);
    blueprint = load_object("requests_target");
}

void inspect();
void next()
{
    clean();
    if (phase == 0)
    {
        object *selection;
        source("int selected_a; int value() { return 11; }\n");
        clones += ({clone_object(blueprint)});
        source("int selected_b; int value() { return 22; }\n");
        clones += ({clone_object(blueprint), clone_object(blueprint)});
        source("int unselected; int value() { return 33; }\n");
        clones += ({clone_object(blueprint)});
        source("int current; int value() { return 44; }\n");
        clones += ({clone_object(blueprint)});
        selection = ({clones[0], clones[0], clones[1], clones[2], blueprint, clones[4]});
        request = update_blueprint(blueprint, selection);
        selection[0] = clones[3];
        destruct(clones[2]);
        require(update_blueprint_result(request)["matched"] == 0,
                "pending poll does not discover or evaluate selection");
    }
    else if (phase == 1 || phase == 2 || phase == 3)
    {
        source("#pragma share_variables\nint retained = 41; int value() { return retained; }\n");
        if (phase == 3) clones += ({clone_object(blueprint)});
        rm("requests_target.c");
        write_file("requests_target.c", "#pragma share_variables\nint retained = 99; int added = 7; int value() { return retained; }\n");
        request = phase == 2 ? update_blueprint(blueprint, ({}))
                            : update_blueprint("requests_target", phase == 3 ? ({blueprint, clones[0]}) : ({}));
    }
    else if (phase == 4)
    {
        source("struct data { int a; }; struct data value;\n");
        rm("requests_target.c");
        write_file("requests_target.c", "struct data { string a; }; struct data value;\n");
        request = update_blueprint("requests_target", ({}));
    }
    else if (phase == 5 || phase == 6)
    {
        source("int value() { return 1; }\n");
        rm("requests_target.c");
        write_file("requests_target.c", phase == 5
            ? "#include \"requests_error.h\"\n"
            : "#pragma warn_dead_code\nint value() { return 2; int unreachable; }\n");
        write_file("requests_error.h", "This is not valid LPC.\n");
        request = update_blueprint("requests_target", ({}));
    }
    else if (phase >= 7 && phase <= 13)
    {
        source("int value() { return 41; } void start_shadow(object ob) { shadow(ob); }\n");
        clones += ({clone_object(blueprint)});
        write_file("requests_side.c", "int auxiliary;\n");
        side = load_object("requests_side");
        write_file("requests_hook.h", "\n");
        rm("requests_target.c");
        write_file("requests_target.c", "#include \"requests_hook.h\"\nint value() { return 42; } void start_shadow(object ob) { shadow(ob); }\n");
        hook_calls = nested = saw_terminal = 0;
        if (phase == 13) clones[0].start_shadow(side);
        request = phase == 8 || phase == 10 || phase == 12
                ? update_blueprint("requests_target", ({clones[0]}))
                : update_blueprint("requests_target");
        if (phase == 7) configure_object(this_object(), OC_HEART_BEAT, 1);
        if (phase == 10 || phase == 11 || phase == 12) clones[0].start_shadow(side);
    }
    else if (phase >= 14 && phase <= 16)
    {
        source("inherit \"empty\"; int value() { return 41; } void request_replace() { replace_program(\"empty\"); }\n");
        clones += ({clone_object(blueprint)});
        if (phase == 14)
        {
            before_replacement = request;
            clones[0].request_replace();
            require(!!catch(update_blueprint(blueprint, ({clones[0]}))),
                    "already pending family replacement rejects admission");
        }
        else if (phase == 15)
        {
            request = update_blueprint(blueprint, ({clones[0]}));
            require(request == before_replacement + 1, "rejected replacement admission does not consume an ID");
            clones[0].request_replace();
        }
        else
        {
            write_file("requests_hook.h", "\n");
            write_file("requests_target.c", "#include \"requests_hook.h\"\n");
            request = update_blueprint("requests_target", ({clones[0]}));
        }
    }
    else if (phase == 17)
    {
        source("#pragma init_variables\nint value() { return 41; }\n");
        write_file("requests_target.c", "int *once = ({1});\n");
        /* The native seam fails a second literal-array materialization.
         * Source-only preparation needs exactly the first one.
         */
        write_file("requests-default-once", "1 1\n");
        request = update_blueprint("requests_target", ({}));
    }
    else if (phase == 18 || phase == 19)
    {
        string before = "#pragma strong_types, save_types\n";
        string after = before;
        for (int i = 0; i < (phase == 18 ? 128 : 129); i++)
        {
            before += sprintf("int v%d = 41;\n", i);
            after += sprintf("string v%d = \"replacement\";\n", i);
        }
        source(before + "int value() { return v0; }\n");
        clones += ({clone_object(blueprint)});
        source(after + "int value() { return 42; }\n");
        request = update_blueprint(blueprint, ({clones[0]}));
    }
    else if (phase == 20 || phase == 21
             || (phase == 22 && file_size("requests-batch-scan-only") >= 0))
    {
        source("inherit \"empty\"; int value() { return 41; } void request_replace() { replace_program(\"empty\"); }\n");
        clones += ({clone_object(blueprint), clone_object(blueprint), clone_object(blueprint)});
        write_file("requests_side.c", "int value() { return 17; }\n");
        side = load_object("requests_side");
        write_file("requests_hook.h", "\n");
        write_file("requests_side.c", "#include \"requests_hook.h\"\n");
        hook_calls = 0;
        /* Both requests are admitted before either hook runs. A's compiler
         * hook queues replacement/destruction in B's already selected family.
         */
        nested = update_blueprint("requests_side", ({}));
        if (phase == 22) write_file("requests-limits", "10000 1\n");
        request = phase == 20 ? update_blueprint("requests_target", ({clones[0], clones[2]}))
                              : update_blueprint("requests_target");
        rm("requests-limits");
        require(request == nested + 1, "cross-family requests preserve the intended batch order");
    }
    else
    {
        msg("BLUEPRINT_REQUESTS: %d checks passed.\n", checks);
        funcall(done, 0);
        return;
    }
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        if (phase == 14)
        {
            require(!function_exists("value", clones[0]) && blueprint.value() == 41,
                    "ordinary deferred replacement proceeds independently of rejected update");
            phase++;
            next();
            return;
        }
        mapping report = update_blueprint_result(request);
        mixed *changes = report["variable_changes"];
        int success = member(({0,1,2,3,6,7,8,11,12,13,17}), phase) >= 0;
        require(report["status"] == (success ? "completed" : "failed"), "strict request outcome");
        if (!success) require(!report["updated"] && !report["blueprint_updated"], "failure preserves every member");
        if (phase == 0)
        {
            require(report["matched"] == 3 && report["already_current"] == 1
                    && report["destroyed"] == 1, "fixed deduplicated surviving clone counts");
            require(sizeof(changes) == 2, "only selected old generations described");
            foreach (mapping change: changes)
                require(member(({"selected_a", "selected_b"}), change["removed"][0]["name"]) >= 0,
                        "caller mutation cannot widen saved selection");
            require(clones[0].value() == 44 && clones[1].value() == 44 && clones[3].value() == 33,
                    "only selected old generations install the loaded candidate");
        }
        else if (phase >= 7)
        {
            if (phase >= 20 && phase <= 22)
            {
                mapping earlier = update_blueprint_result(nested);
                require(blueprint_outcome(report) == (phase == 22 ? "SELECTION_SCAN_LIMIT" : "REPLACEMENT_PENDING")
                        && report["matched"] == (phase == 22 ? 0 : phase == 20 ? 1 : 2)
                        && report["destroyed"] == (phase == 20),
                        "earlier batch hook replacement retains complete source-path selection counts");
                if (phase == 22)
                    require(strstr(report["errors"][0]["message"], "counts are unavailable") >= 0,
                            "incomplete final scan publishes no prefix or stale selection counts");
                require(hook_calls == 1 && blueprint_outcome(earlier) == "completed"
                        && earlier["completed_at"] == report["completed_at"],
                        "both independent requests terminate in the same detached batch");
                require(blueprint.value() == 41 && clones[1].value() == 41 && side.value() == 17,
                        "batch preparation preserves both live blueprints and unselected clone values");
                require(clones[0] && !function_exists("value", clones[0]),
                        "ordinary queued replacement proceeds independently after request processing");
                phase++; next(); return;
            }
            else if (phase == 18 || phase == 19)
            {
                mixed *blocks = changes[0]["blockers"];
                require(blueprint_outcome(report) == (phase == 18
                        ? "SCHEMA_INCOMPATIBLE" : "SCHEMA_DIAGNOSTIC_LIMIT"),
                        "schema blocker cap and overflow have distinct explicit outcomes");
                require(sizeof(blocks) == (phase == 18 ? 128 : 129)
                        && blocks[127]["name"] == "v127",
                        "all 128 bounded declaration blockers remain available");
                if (phase == 19)
                    require(blocks[128]["code"] == "SCHEMA_DIAGNOSTIC_LIMIT"
                            && strstr(blocks[128]["message"], "incomplete") >= 0
                            && sizeof(filter(report["errors"], (: $1["code"] == "SCHEMA_DIAGNOSTIC_LIMIT"
                                && $1["role"] == "generation" :))) == 1,
                            "schema truncation is explicit in generation and flattened errors");
                require(report["matched"] == 1 && clones[0].value() == 41
                        && blueprint.value() == 42 && sizeof(changes[0]["matched"]) == (phase == 18 ? 128 : 129),
                        "schema overflow retains declaration evidence and unchanged live values");
                phase++; next(); return;
            }
            else if (phase == 17)
            {
                require(blueprint_outcome(report) == "completed"
                        && sizeof(changes) == 1 && changes[0]["added"][0]["default"]["value"][0] == 1,
                        "source-only full schema reuses the single prepared literal");
                require(!report["matched"] && blueprint.value() == 41,
                        "source-only default preparation leaves live state unchanged");
                phase++;
                next();
                return;
            }
            else if (phase >= 15)
            {
                require(member(({"REPLACEMENT_PENDING", "UNSUPPORTED_OBJECT"}), blueprint_outcome(report)) >= 0,
                        "replacement queued after admission or inside compiler hook blocks preparation");
                require(report["matched"] == 1 && !report["destroyed"],
                        "replacement conflict retains the complete surviving selection count");
                require(blueprint.value() == 41, "replacement conflict leaves the source blueprint unchanged");
                phase++;
                next();
                return;
            }
            else if (phase >= 11)
            {
                require(hook_calls == 1 && !clones[0] && !report["matched"]
                        && report["destroyed"] == (phase == 12),
                        "compiler hook removal determines surviving execution selection");
                require(blueprint_outcome(report) == "completed",
                        "a removed unsupported clone cannot remain an execution blocker");
            }
            else if (phase == 7 || phase == 8)
            {
                require(hook_calls == 1 && report["matched"] == (phase == 7 ? 2 : 1),
                        "all discovery follows compiler hooks; explicit membership never widens");
                require(blueprint_outcome(report) == "completed",
                        "unselected unsupported survivor does not block explicit request");
                if (phase == 7)
                    require(saw_terminal, "heartbeat sees terminal request while compiler-hook submission waits next tick");
            }
            else
            {
                require(blueprint_outcome(report) == "UNSUPPORTED_OBJECT",
                        sprintf("unsupported surviving selection fails strictly: %d %O", phase, blueprint_outcome(report)));
                require(report["matched"] == (phase == 9 ? 2 : 1),
                        "unsupported survivors still have complete surviving selection counts");
            }
            require(blueprint.value() == (success ? 42 : 41)
                    && (!clones[0] || clones[0].value() == (success ? 42 : 41)),
                    "post-hook success installs and rejection preserves the family");
        }
        else if (phase == 5 || phase == 6)
        {
            mixed *diagnostics = report["errors"][(phase == 6 ? 0 : 1)..];
            require(sizeof(diagnostics) > 0, "compiler diagnostics survive private context cleanup");
            require(diagnostics[0]["line"] > 0 && sizeof(diagnostics[0]["message"])
                    && diagnostics[0]["warning"] == (phase == 6), "compiler diagnostic location and severity");
            if (phase == 5)
                require(diagnostics[0]["file"] == "/requests_error.h"
                        && blueprint_outcome(report) == "COMPILE_FAILED", sprintf("include syntax diagnostic and failure reason: %O %O", diagnostics[0]["file"], blueprint_outcome(report)));
            else
                require(blueprint_outcome(report) == "completed", "warning preserves successful preparation");
            diagnostics[0]["message"] = "tampered";
            require(update_blueprint_result(request)["errors"][phase == 6 ? 0 : 1]["message"] != "tampered",
                    "diagnostic copy isolation");
            require(blueprint.value() == (phase == 6 ? 2 : 1), "warning-only compilation installs; syntax failure preserves code");
            rm("requests_error.h");
        }
        else if (phase == 2)
            require(!sizeof(changes) && !report["matched"], "object empty selection has no implicit blueprint");
        else
        {
            require(sizeof(changes) == 1 && changes[0]["blueprint"], "source blueprint described exactly once");
            require(report["matched"] == (phase == 3) && !report["already_current"], "blueprint excluded from clone counts");
            if (phase == 4)
            {
                require(report["candidate_generation"] > 0
                        && member(map(changes[0]["blockers"], (: $1["code"] :)), "STRUCT_LAYOUT_CHANGED") >= 0,
                        "source-only incompatible struct retains candidate and layout evidence");
                require(blueprint_outcome(report) == "SCHEMA_INCOMPATIBLE", "schema failure is explicit");
                require(member(map(report["errors"][1..], (: $1["code"] :)), "STRUCT_LAYOUT_CHANGED") >= 0,
                        "errors contains generation-specific schema blockers");
            }
            else
            {
                require(changes[0]["blueprint_defaults"][0]["default"]["value"] == 7,
                        "blueprint initializer decision retained");
                require(blueprint.value() == 41, "private preparation preserves blueprint values");
                if (phase == 3)
                    require(changes[0]["added"][0]["default"]["kind"] == "shared",
                            "same generation exposes distinct clone sharing decision");
            }
        }
        phase++;
        next();
    }); publish);
    if (err) { clean(); funcall(done, 1); }
}

void run(closure callback)
{
#ifndef __BLUEPRINT_UPDATE_TESTING__
    if (file_size("requests-batch-scan-only") >= 0)
    {
        msg("BLUEPRINT_INSTRUMENTED: reduced batch scan requires test build; skipped.\n");
        funcall(callback, 0);
        return;
    }
#endif
    done = callback;
    if (file_size("requests-removal-only") >= 0) phase = 11;
    if (file_size("requests-single-default-only") >= 0) phase = 17;
    if (file_size("requests-blockers-only") >= 0) phase = 18;
    if (file_size("requests-replacement-only") >= 0) phase = 16;
    if (file_size("requests-batch-only") >= 0) phase = 20;
    if (file_size("requests-batch-all-only") >= 0) phase = 21;
    if (file_size("requests-batch-scan-only") >= 0) phase = 22;
    next();
}
#endif
