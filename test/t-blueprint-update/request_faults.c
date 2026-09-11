#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/gc.inc"
#include "/sys/rtlimits.h"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, previous;
int request, checks, point, phase;
void require(int condition, string message)
{
    checks++;
    if (!condition) raise_error("Request faults: " + message + "\n");
}
void cleanup()
{
    if (previous) destruct(previous);
    if (blueprint) destruct(blueprint);
    rm("requests_fault_target.c"); rm("requests-poll-fault");
    rm("requests-budget"); rm("requests-limits"); rm("requests-poll-budget");
}
void collected(int failed)
{
    mixed err = catch(funcall(function void()
    {
        require(!failed, "poll unwinds and terminal disposal pass GC/refcounts");
        require(update_blueprint_result(request)["errors"][0]["code"] == "REPORT_SIZE_LIMIT",
                "minimal overflow explanation survives GC and program retirement");
        msg("BLUEPRINT_REQUEST_FAULTS: %d checks passed; %d poll allocation boundaries.\n", checks, point);
    }); publish);
    cleanup(); funcall(done, !!err);
}
void inspect();
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        if (!phase)
        {
            require(report["matched"] == 2, "clone quota applies after deduplication and blueprint exclusion");
            require(sizeof(report["variable_changes"]) == 1, "poll fixture has a complete schema tree");
            write_file("requests-poll-budget", "32768\n");
            require(!!catch(update_blueprint_result(request)),
                    "whole-copy budget includes native identity table bookkeeping");
            rm("requests-poll-budget");
            require(update_blueprint_result(request)["matched"] == 2,
                    "copy budget failure leaves complete cache retryable");
            for (point = 0; point < 2000; point++)
            {
                mixed failed;
                rm("requests-poll-fault"); write_file("requests-poll-fault", sprintf("%d\n", point));
                failed = catch(update_blueprint_result(request));
                rm("requests-poll-fault");
                if (!failed)
                {
                    require(point > 50, "poll allocation faults actually reached rooted tree boundaries");
                    break;
                }
                require(update_blueprint_result(request)["variable_changes"][0]["added"][0]["name"] == "payload",
                        "failed poll leaves complete cache retryable");
            }
            require(point < 2000, "bounded complete poll fault sweep");
            require(!!catch(limited((: update_blueprint_result(request) :), LIMIT_EVAL, 40)),
                    "evaluation exhaustion throws instead of returning a partial report");
            require(update_blueprint_result(request)["matched"] == 2, "evaluation exhaustion leaves cached report unchanged");
            require(!!catch(limited((: update_blueprint_result(request) :), LIMIT_ARRAY, 1)),
                    "normal array limits apply to fresh report values");
            require(!!catch(limited((: update_blueprint_result(request) :), LIMIT_MAPPING_KEYS, 1)),
                    "normal mapping limits apply to fresh report values");
            /* The first attempt now migrated previous. Load a newer candidate
             * so the next request still needs an actual schema/report plan.
             */
            string next_source = read_file("requests_fault_target.c") + "int another=2;\n";
            destruct(blueprint); rm("requests_fault_target.c");
            write_file("requests_fault_target.c", next_source);
            blueprint=load_object("requests_fault_target");
            rm("requests-budget"); write_file("requests-budget", "2000\n");
            request = update_blueprint(blueprint, ({previous}));
            phase = 1;
            call_out(#'inspect, __ALARM_TIME__ + 1);
            return;
        }
        require(report["errors"][0]["code"] == "REPORT_SIZE_LIMIT"
                && strstr(report["errors"][0]["message"], "incomplete") >= 0,
                "whole report overflow retains an explicit incomplete explanation");
        cleanup();
        start_gc(#'collected);
    }); publish);
    if (err) { cleanup(); funcall(done, 1); }
}
void run(closure callback)
{
#ifndef __BLUEPRINT_UPDATE_TESTING__
    msg("BLUEPRINT_INSTRUMENTED: request_faults.c requires test build; skipped.\n");
    funcall(callback, 0); return;
#endif
    object other;
    done = callback;
    cleanup();
    write_file("requests_fault_target.c", "int version() { return 1; }\n");
    blueprint = load_object("requests_fault_target");
    previous = clone_object(blueprint);
    other = clone_object(blueprint);
    destruct(blueprint); rm("requests_fault_target.c");
    write_file("requests_fault_target.c", "#pragma init_variables\nmixed payload = ({ ([ ({1, 2}): ({\"alpha\", ({3, 4})}) ]), ({5, 6}) }); int version() { return 2; }\n");
    blueprint = load_object("requests_fault_target");
    write_file("requests-limits", "2 4\n");
    require(!!catch(update_blueprint(blueprint, ({previous, previous, other, other, blueprint}))),
            "raw examination has an independent limit");
    request = update_blueprint(blueprint, ({blueprint, previous, previous, other}));
    rm("requests-limits");
    write_file("requests-poll-budget", "1\n");
    require(!!catch(update_blueprint_result(request)),
            "pending report root and metadata share the whole-copy budget");
    rm("requests-poll-budget");
    require(update_blueprint_result(request)["status"] == "pending",
            "failed pending copy leaves request membership and state unchanged");
    /* Keep the second selected identity owned until execution, then let the
     * source/program retirement path release it with the rest of the family.
     */
    call_out(function void() { if (other) destruct(other); }, __ALARM_TIME__ + 2);
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
#endif
