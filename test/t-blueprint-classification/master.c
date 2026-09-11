#include "/inc/base.inc"
#ifdef __BLUEPRINT_UPDATE__
int request, budget = 65;
object source;
string *get_simul_efun()
{
    write_file("sefun.c", "int sefun_probe(){return 1;}\n", 1);
    load_object("sefun");
    return ({"/sefun"}) + allocate(20, "/absent_backup");
}
void submit_case();
void inspect()
{
    mapping report = update_blueprint_result(request);
#ifndef __BLUEPRINT_UPDATE_TESTING__
    if (report["status"] != "completed" || !report["blueprint_updated"])
    { shutdown(1); return; }
    msg("CLASSIFICATION_INCOMPLETE: ordinary update completed; work controls unavailable.\n");
    shutdown(0);
    return;
#endif
    msg("CLASSIFICATION_BUDGET: %d %O\n", budget, report["errors"]);
    if (report["status"] == "failed" && report["errors"][0]["code"] == "RUNTIME_DEPENDENCY_LIMIT")
    {
        if (!report["errors"][0]["incomplete"])
        {
            msg("CLASSIFICATION_INCOMPLETE: missing marker after bounded source classification.\n");
            shutdown(1);
        }
        else
        {
            msg("CLASSIFICATION_INCOMPLETE: bounded source classification has explicit incomplete evidence.\n");
            shutdown(0);
        }
        return;
    }
    if ((budget += 5) > 180) { msg("CLASSIFICATION_INCOMPLETE: intended boundary not reached.\n"); shutdown(2); return; }
    submit_case();
}
void submit_case()
{
    write_file("requests-limits", sprintf("10000 1000000 %d\n", budget), 1);
    request = update_blueprint("classification_target", ({}));
    rm("requests-limits");
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
#endif
string *epilog(int flag)
{
#ifdef __BLUEPRINT_UPDATE__
    write_file("classification_target.c", "int retained;\n", 1);
    source = load_object("classification_target");
    call_out(#'shutdown, 120, 3);
    submit_case();
#else
    msg("CLASSIFICATION_INCOMPLETE: feature disabled.\n");
    shutdown(0);
#endif
    return 0;
}
