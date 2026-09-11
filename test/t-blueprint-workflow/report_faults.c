#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/sys/driver_info.h"
#if defined(__BLUEPRINT_UPDATE__)
object source, first, second;
closure *held;
closure done;
int request, point, checks, recovering, collected, gc_checks, gc_skipped;
void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Report allocation: " + why + "\n");
}
void finish(int failed)
{
    held = 0;
    foreach (object ob : ({first,second,source})) if (ob) destruct(ob);
    rm("report_fault_target.c"); rm("requests-runtime-fault"); rm("report-fault.gc.log");
    funcall(done, failed);
}
void submit();
void inspect_gc(int retry)
{
    string evidence = read_file("report-fault.gc.log");
    if (!stringp(evidence) && !retry)
    {
        call_out(#'inspect_gc, __ALARM_TIME__ + 1, 1);
        return;
    }
    if (catch(
        require(stringp(evidence) && strstr(evidence, "--- Garbage Collection ---") >= 0
                && strstr(evidence, "GC freed ") >= 0, "fresh completed collection evidence"),
        require(!sizeof(regexp(explode(evidence, "\n"),
                              "freeing.*block|tabled string.*was left unreferenced")), "collection has no lost blocks"); publish))
    { finish(1); return; }
    msg(evidence);
    rm("report-fault.gc.log");
    collected++;
    submit();
}
void collect()
{
    gc_checks++;
    rm("report-fault.gc.log");
    if (driver_info(DI_MEMORY_ALLOCATOR_NAME) == "system malloc")
    {
        gc_skipped++;
        msg("BLUEPRINT_REPORT_GC: collection unsupported by sysmalloc; runtime checks continue.\n");
        submit();
        return;
    }
    garbage_collection("report-fault.gc.log");
    call_out(#'inspect_gc, __ALARM_TIME__ + 1, 0);
}
void inspect()
{
    mapping report = update_blueprint_result(request);
    rm("requests-runtime-fault");
    if (catch(
        require(report["matched"] == 2, "complete counts"),
        require(first.value() == 41 && second.value() == 41, "no partial migration"),
        require(sizeof(report["variable_changes"]) == 1
             && report["variable_changes"][0]["removed"][0]["name"] == "obsolete", "completed schema evidence survives diagnostic allocation failure"); publish))
    { finish(1); return; }
    if (recovering)
    {
        if (catch(require(report["status"] == "completed" && report["updated"] == 2, "recovery after all diagnostic faults"); publish))
        { finish(1); return; }
        if (catch(require(!point || (gc_checks > 0
                     && (driver_info(DI_MEMORY_ALLOCATOR_NAME) == "system malloc"
                         ? !collected && gc_skipped == gc_checks
                         : collected > 0 && collected == gc_checks)), "actual and unsupported collection accounting"); publish))
        { finish(1); return; }
        msg("BLUEPRINT_REPORT_FAULTS: %d checks, %d injected boundaries, %d GC checks, %d actual collections, %d unsupported and successful recovery.\n", checks, point, gc_checks, collected, gc_skipped);
        finish(0); return;
    }
    if (catch(require(blueprint_outcome(report) == "LIVE_CLOSURE" && !report["updated"], "primary reason and rollback retained"); publish))
    { finish(1); return; }
    if (!report["errors"][0]["incomplete"])
    {
        if (catch(require(sizeof(filter(report["errors"], (: $1["object"] == object_name(first) :))) == 1
                       && sizeof(filter(report["errors"], (: $1["object"] == object_name(second) :))) == 1, "successful materialization identifies both instances"); publish))
        { finish(1); return; }
        recovering = 1; held = 0; submit(); return;
    }
    if (catch(require(report["errors"][0]["incomplete_code"] == "PREPARATION_FAILED", "explicit diagnostic incompleteness"); publish))
    { finish(1); return; }
    if (++point > 100) { finish(1); return; }
    if (!(point % 7)) collect(); else submit();
}
void submit()
{
    if (!recovering) write_file("requests-runtime-fault", sprintf("%d\n", point), 1);
    request = update_blueprint("report_fault_target", ({first,second}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
void run(closure callback)
{
    done = callback;
    if (catch(
        write_file("report_fault_target.c", "int retained = 41, obsolete = 17; int value(){return retained;} closure handle(){return function int(){return retained;};}\n", 1),
        source = load_object("report_fault_target"), first = clone_object(source), second = clone_object(source),
        held = ({first.handle(),second.handle()}),
        write_file("report_fault_target.c", "int added = 19, retained = 99; int value(){return retained;} closure handle(){return function int(){return retained;};}\n", 1),
        submit(); publish)) finish(1);
}
#endif
