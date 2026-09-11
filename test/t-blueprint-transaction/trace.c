#include "/inc/base.inc"
#ifdef __BLUEPRINT_UPDATE__
object source, target;
closure done;
int request, retries;
void inspect()
{
    mapping report=update_blueprint_result(request);
    if(report["status"]=="pending" && retries++<12) { call_out("inspect",1);return; }
    mixed failed=catch(funcall(function void()
    {
        if(report["status"]!="completed" || target.version()!=2)
            raise_error("Trace cohort did not install.\n");
#if __EFUN_DEFINED__(last_instructions)
        last_instructions(1000000,0);
        last_instructions(1000000,1);
        catch(raise_error("expected error trace after old-program release\n");publish);
#endif
        msg("BLUEPRINT_TRACE: installed cohort, both verbosity modes and error trace.\n");
    });publish);
    destruct(target);destruct(source);rm("trace_target.c");
    funcall(done,!!failed);
}
void run(closure callback)
{
    done=callback;
    rm("trace_target.c");
    write_file("trace_target.c","int version(){int n;for(int i=0;i<50;i++)n+=i;return 1;}\n");
    source=load_object("trace_target");target=clone_object(source);
    rm("trace_target.c");write_file("trace_target.c","int version(){return 2;}\n");
    /* Seed old instructions immediately before admission; no old clone or
     * indexed handle will keep this generation alive after the transaction.
     */
    source.version();target.version();
#if __EFUN_DEFINED__(last_instructions)
    if(strstr(implode(last_instructions(1000000,1),"\n"),"trace_target")<0)
        raise_error("Old program is missing from the trace fixture.\n");
#endif
    request=update_blueprint("trace_target");
    call_out("inspect",__ALARM_TIME__+1);
}
#endif
