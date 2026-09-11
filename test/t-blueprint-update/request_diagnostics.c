#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint;
int request, checks, point, phase;

void require(int condition, string message)
{
    checks++;
    if (!condition) raise_error("Request diagnostics: " + message + "\n");
}
void clean()
{
    if (blueprint) destruct(blueprint);
    rm("requests_diag_target.c"); rm("requests-diagnostic-fault"); rm("requests-budget");
}
void next();
void collected(int failed)
{
    mixed err = catch(
        require(!failed, "diagnostic partial rows and compiler contexts released after GC"),
        require(update_blueprint_result(request)["errors"][0]["code"] == "REPORT_SIZE_LIMIT",
                "minimal diagnostic budget failure survives context and source retirement"); publish);
    if (!err) msg("BLUEPRINT_REQUEST_DIAGNOSTICS: %d checks passed; %d construction boundaries.\n", checks, point);
    clean(); funcall(done, !!err);
}
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        string code = blueprint_outcome(report);
        require(!report["updated"] && blueprint.value() == (code == "completed" ? 99 : 41),
                "diagnostic success installs; construction failures preserve code");
        if (!phase)
        {
            if (code == "completed")
            {
                require(point > 10 && point < 64, "diagnostic faults reached every rooted row boundary");
                require(sizeof(report["errors"]) == 2, "both ordinary warnings survive final compiler cleanup");
                phase = 1;
            }
            else
            {
                require(code == "REPORT_ALLOCATION_FAILED", "diagnostic allocation failure has a precise minimal reason");
                require(!sizeof(report["variable_changes"]) && !report["candidate_generation"],
                        "failed rich construction never exposes a partial schema as complete");
                point++;
                require(point < 64, "finite diagnostic allocation fault sweep");
            }
        }
        else if (phase == 1)
        {
            require(code == "COMPILE_DIAGNOSTIC_LIMIT" && sizeof(report["errors"]) == 129,
                    "actual 128-row diagnostic cap has an explicit truncation reason");
            require(!report["candidate_generation"] && !sizeof(report["variable_changes"]),
                    "diagnostic overflow never publishes a candidate or partial schema");
            phase++;
        }
        else
        {
            require(code == "REPORT_SIZE_LIMIT" && sizeof(report["errors"]) == 1,
                    "zero diagnostic output budget retains only the reserved failure explanation");
            clean();
            start_gc(#'collected);
            return;
        }
        next();
    }); publish);
    if (err) { clean(); funcall(done, 1); }
}
void next()
{
    string source = "#pragma warn_dead_code\nint value() { return 99; }\n";
    int warnings = phase == 1 ? 129 : 2;
    clean();
    write_file("requests_diag_target.c", "int value() { return 41; }\n");
    blueprint = load_object("requests_diag_target");
    rm("requests_diag_target.c");
    for (int i = 0; i < warnings; i++)
        source += sprintf("int warning%d() { return 1; int unreachable; }\n", i);
    write_file("requests_diag_target.c", source);
    if (!phase) write_file("requests-diagnostic-fault", sprintf("%d\n", point));
    if (phase == 2) write_file("requests-budget", "0\n");
    request = update_blueprint("requests_diag_target", ({}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
void run(closure callback)
{
#ifndef __BLUEPRINT_UPDATE_TESTING__
    msg("BLUEPRINT_INSTRUMENTED: request_diagnostics.c requires test build; skipped.\n");
    funcall(callback, 0); return;
#endif
    done = callback;
    next();
}
#endif
