#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#ifdef __BLUEPRINT_UPDATE__
object source, target;
closure stale;
int request;
void inspect()
{
    mapping report=update_blueprint_result(request);
    if(report["status"]!="completed") raise_error(sprintf("inactive update failed: %O\n",report));
    mixed err=catch(sprintf("%O",stale));
    // A dead security binding is uncallable, but diagnostics must stay safe.
    if(stale && funcall(stale)!=0) raise_error("dead security binding executed\n");
    msg("BLUEPRINT_INACTIVE_BINDING: safe diagnostic=%O.\n",err);
    rm("inactive_target.c");shutdown(0);
}
void run_test()
{
    object dead;
    call_out(#'shutdown,45,1);
    rm("inactive_target.c");write_file("inactive_target.c","int removed(){return 19;}\n");
    source=load_object("inactive_target");target=clone_object(source);
    dead=clone_object(this_object());
    stale=bind_lambda(symbol_function("removed",target),dead);destruct(dead);
    // Preserve the stale closure allocation without a GC cleanup in between.
    msg("BLUEPRINT_INACTIVE_BEFORE: %O\n",stale);
    rm("inactive_target.c");write_file("inactive_target.c","int fresh(){return 23;}\n");
    request=update_blueprint("inactive_target");call_out(#'inspect,__ALARM_TIME__+1);
}
#else
void run_test(){shutdown(0);}
#endif
string *epilog(int eflag){if(catch(run_test();publish))shutdown(1);return 0;}
