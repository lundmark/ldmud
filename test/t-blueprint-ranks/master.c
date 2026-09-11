#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"
#include "/inc/deep_eq.inc"
#ifdef __BLUEPRINT_UPDATE__
object target;
closure alpha, transient, later;
mapping keys;
int request, phase, checks;
void require(int ok,string why){checks++;if(!ok)raise_error("Binding ranks: "+why+"\n");}
string body(int version)
{
    return "#pragma strong_types, save_types\n"
        + (version==2 || version==4 ? "int transient(){return 19;}\n" : "")
        + (version>=2 ? "int later(){return 23;}\n" : "")
        + "int alpha(){return 41;}\n"
        + "closure fixed(){return #'alpha;} closure pick(int n){return "
        + (version==1 ? "0" : version==3 ? "n ? #'later : 0" : "n ? #'later : #'transient")+";}\n";
}
void submit(int version);
void collected(int failed)
{
    require(!failed,"binding rank GC roots");
    if(phase==1)
    {
        require(deep_eq(m_indices(keys),({alpha,transient,later})),"new declarations follow original comparison ranks");
        m_delete(keys,transient);transient=0;
        submit(3);
    }
    else
    {
        require(deep_eq(m_indices(keys),({alpha,later,transient})),"removed and reintroduced declaration never reuses an old rank");
        require(keys[alpha]==1 && keys[later]==3 && keys[transient]==4,"all stored keys remain accessible");
        msg("BLUEPRINT_NAMED_RANKS: %d checks passed.\n",checks);
        rm("rank_target.c");shutdown(0);
    }
}
void inspect()
{
    if(catch(funcall(function void()
    {
        mapping report=update_blueprint_result(request);
        require(report["status"]=="completed",sprintf("rank migration: %O",report["errors"]));
        require(alpha==target.fixed() && funcall(alpha)==41,"oldest rank keeps fresh equivalent identity");
        phase++;
        if(phase==1)
        {
            transient=target.pick(0);later=target.pick(1);
            keys[transient]=2;keys[later]=3;start_gc(#'collected);
        }
        else if(phase==2)
        {
            require(!target.pick(0) && funcall(later)==23,"unheld declaration is removed while later handle survives");
            submit(4);
        }
        else
        {
            transient=target.pick(0);keys[transient]=4;
            require(later==target.pick(1),"later declaration keeps its existing rank");
            start_gc(#'collected);
        }
    });publish))shutdown(1);
}
void submit(int version)
{
    rm("rank_target.c");write_file("rank_target.c",body(version));
    request=update_blueprint("rank_target");call_out(#'inspect,__ALARM_TIME__+1);
}
void run_test()
{
    call_out(#'shutdown,70,1);rm("rank_target.c");write_file("rank_target.c",body(1));
    target=clone_object("rank_target");alpha=target.fixed();keys=([alpha:1]);submit(2);
}
#else
void run_test(){shutdown(0);}
#endif
string *epilog(int eflag){if(catch(run_test();publish))shutdown(1);return 0;}
