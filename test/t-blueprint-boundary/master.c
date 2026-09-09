#include "/inc/base.inc"
#include "/inc/gc.inc"
#include "/sys/configuration.h"

#ifdef __BLUEPRINT_UPDATE__
object a,b,c,d,e,owner,first,second;
int aid,bid,cid,did,eid,phase,checks,attempts,hooks,saw_heartbeat,saw_callout;

void require(int ok,string label)
{
    checks++;
    if(!ok) { msg("BOUNDARY FAILURE: %s\n",label);shutdown(1);raise_error(label+"\n"); }
}

mixed include_file(string name,string from,int system)
{
    if(name!="boundary_hook.h") return 0;
    hooks++;
    require(update_blueprint_result(aid)["status"]=="pending"
            &&update_blueprint_result(bid)["status"]=="pending", "detached remainder stays rooted during compiler hook");
    require(!this_player(),"compiler hook has explicit idle security context");
    cid=update_blueprint(c,({}));
    garbage_collection();
    if(!phase) raise_error("expected throwing compiler hook\n");
    destruct(owner);
    return 0;
}

void heart_beat()
{
    if(!cid || saw_heartbeat) return;
    require(update_blueprint_result(bid)["status"]=="completed" && first.version()==2 && second.version()==2,
            "unrelated B completes after A fails and before heartbeat");
    require(update_blueprint_result(cid)["status"]=="pending", "compiler-hook C waits for next tick");
    if(!phase) require(update_blueprint_result(aid)["status"]=="failed", "throwing A terminates once");
    else require(!!catch(update_blueprint_result(aid)),"destroyed owner cancels precommit A");
    did=update_blueprint(d,({}));
    saw_heartbeat=1;
    call_out("nested_callout",0);
    configure_object(this_object(),OC_HEART_BEAT,0);
}

void nested_callout()
{
    require(saw_heartbeat && update_blueprint_result(bid)["status"]=="completed", "callout follows the completed batch and heartbeat");
    require(update_blueprint_result(did)["status"]=="pending", "heartbeat submission still pending in same-tick callout");
    eid=update_blueprint(e,({}));
    saw_callout=1;
}

void next_case();
void collected(int failed)
{
    require(!failed,"terminal outcomes survive deferred and subsequent explicit GC");
    foreach(object ob: ({first,second,a,b,c,d,e,owner})) if(ob) destruct(ob);
    foreach(string name: ({"a","b","c","d","e"})) rm(name+".c");
    rm("boundary_hook.h");
    if(!phase++) next_case();
    else { msg("BLUEPRINT_BOUNDARY: %d checks passed.\n",checks);load_object("selftest").run(#'shutdown); }
}

void poll()
{
    if((!eid || update_blueprint_result(eid)["status"]=="pending") && attempts++<12)
    { call_out("poll",1);return; }
    require(saw_heartbeat && saw_callout && hooks==1,"one compiler attempt and both dispatch boundaries observed");
    foreach(int id: ({cid,did,eid}))
        require(update_blueprint_result(id)["status"]=="completed"
                &&update_blueprint_result(id)["completed_at"]>=update_blueprint_result(bid)["completed_at"],
                "submissions observed pending at dispatch boundaries later complete");
    require(first.value()==37 && second.value()==61,"final capture uses post-submission live values");
    require(a.version()==1,"precommit A failure preserves old behavior");
    start_gc(#'collected);
}

void next_case()
{
    cid=did=eid=attempts=hooks=saw_heartbeat=saw_callout=0;
    foreach(string name: ({"a","b","c","d","e"}))
    { rm(name+".c");write_file(name+".c","int retained;int version(){return 1;}int value(){return retained;}void seed(int n){retained=n;}\n"); }
    a=load_object("a");b=load_object("b");c=load_object("c");d=load_object("d");e=load_object("e");
    first=clone_object(b);second=clone_object(b);
    rm("a.c");write_file("boundary_hook.h","\n");
    write_file("a.c","#include \"boundary_hook.h\"\nint retained;int version(){return 2;}\n");
    rm("b.c");write_file("b.c","int retained;int version(){return 2;}int value(){return retained;}\n");
    owner=clone_object("owner");
    aid=owner.submit("a");bid=update_blueprint("b");
    first.seed(37);second.seed(61);
    configure_object(this_object(),OC_HEART_BEAT,1);
    call_out("poll",__ALARM_TIME__+1);
}
#endif

string *epilog(int flag)
{
#ifdef __BLUEPRINT_UPDATE__
    call_out("timeout",90);next_case();
#else
    msg("BLUEPRINT_BOUNDARY: feature disabled; skipped.\n");shutdown(0);
#endif
    return 0;
}
void timeout(){shutdown(1);}
