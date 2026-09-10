#define OWN_PRIVILEGE_VIOLATION
#include "/inc/base.inc"
object driver_owner;
object submitter() { return driver_owner; }
#ifdef __BLUEPRINT_UPDATE__
mapping observe(int id) { return update_blueprint_result(id); }
#endif
int privilege_violation(string op, mixed who, mixed a, mixed b)
{
    if (op == "update_blueprint") driver_owner = who;
    return 1;
}
int current;
#ifdef TASK33_CASE
string *cases = ({TASK33_CASE});
#else
string *cases = ({"reports", "advanced", "coordinator", "report_faults", "python_reports"});
#endif
int query_allow_shadow(object ob) { return 1; }
void next(int failed)
{
    if (failed) { shutdown(1); return; }
    if (current == sizeof(cases))
    {
        msg("BLUEPRINT_WORKFLOW: %d cases passed.\n", current);
        shutdown(0); return;
    }
    load_object(cases[current++]).run(#'next);
}
string *epilog(int flag)
{
#ifdef __BLUEPRINT_UPDATE__
    call_out(#'shutdown, 600, 1);
    if (catch(next(0); publish)) shutdown(1);
#else
    msg("BLUEPRINT_WORKFLOW: feature disabled.\n");
    shutdown(0);
#endif
    return 0;
}
