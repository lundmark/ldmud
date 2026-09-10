#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
object source, first, second;
closure done;
int request, checks;
void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Python report: " + why + "\n");
}
void finish(int failed)
{
    report_python_drop();
    foreach (object ob : ({first,second,source})) if (ob) destruct(ob);
    rm("python_report_target.c");
    funcall(done, failed);
}
void inspect()
{
    mapping report = update_blueprint_result(request);
    if (catch(require(blueprint_outcome(report) == "PYTHON_HANDLE" && report["matched"] == 2
                   && !report["updated"], "Python iterators block atomically"); publish))
    { finish(1); return; }
    foreach (object ob : ({first,second}))
    {
        mapping *rows = filter(report["errors"], (: $1["object"] == object_name(ob) :));
        if (catch(
            require(sizeof(rows) == 3, "iterator and both Python-only named declarations reported"),
            require(sizeof(filter(rows, (: $1["code"] == "PYTHON_HANDLE" :))) == 1, "separate unsupported Python reason"),
            require(sizeof(filter(rows, (: $1["kind"] == "variable" && $1["name"] == "obsolete" :))) == 1, "Python variable identity"),
            require(sizeof(filter(rows, (: $1["kind"] == "function" && $1["name"] == "discarded" :))) == 1, "Python function identity"); publish))
        { msg("PYTHON_REPORT_EVIDENCE: %O\n", report); finish(1); return; }
    }
    msg("BLUEPRINT_PYTHON_REPORTS: %d checks, all Python-only declarations and iterators reported.\n", checks);
    finish(0);
}
void run(closure callback)
{
    done = callback;
    if (catch(
        write_file("python_report_target.c", "int obsolete = 17; int discarded(){return obsolete;} int value(){return 41;}\n", 1),
        source = load_object("python_report_target"), first = clone_object(source), second = clone_object(source),
        report_python_hold(first, second),
        write_file("python_report_target.c", "int value(){return 41;}\n", 1),
        request = update_blueprint("python_report_target", ({first,second})),
        call_out(#'inspect, __ALARM_TIME__ + 1); publish)) finish(1);
}
#else
void run(closure callback)
{
    msg("BLUEPRINT_PYTHON_REPORTS: feature or Python disabled.\n");
    funcall(callback, 0);
}
#endif
