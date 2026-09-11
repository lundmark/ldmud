#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"
#include "/inc/deep_eq.inc"
#ifdef __BLUEPRINT_UPDATE__
object source,target;
closure *held,*ordered;
closure rebound;
mapping keys,rebuilt;
string *saved;
int request,checks;

void require(int ok,string why) { checks++; if(!ok) raise_error("Inherited handles: "+why+"\n"); }
string body(int v)
{
    return "#pragma strong_types, save_types, init_variables\n"
        "inherit \"left\"; inherit \"right\";\n"
        + (v==1 ? "int own=41;\n" : "int added=7; int own=41;\n")
        + (v==2 ? "int extra(){return 7;}\n" : "")
        + "int result(){return own+"+v+";}\n"
        + "closure *handles(){return ({#'result,#'left::result,#'right::result,#'left::common,#'right::common,#'common,left::cell(),right::cell()});}\n"
        + "object *identity(){return ({this_object(),previous_object()});} closure identity_handle(){return #'identity;}\n"
        + "string saved(int i){return save_value(handles()[i]);} closure restored(string data){return restore_value(data);}\n"
        + "closure normal(){return symbol_function(\"result\",this_object());}\n"
        + "void replace_me(){replace_program(\"left\");}\n";
}
void inspect_handles(int v)
{
    closure *fresh=target.handles();

    require(funcall(held[0])==41+v && funcall(held[1])==11 && funcall(held[2])==23,
            "normal override and explicit parent dispatch retain their declarations");
    require(funcall(held[3])==101 && funcall(held[4])==101 && funcall(held[5])==101,
            "repeated virtual inherited dispatch stays callable");
    require(funcall(held[6])==11 && funcall(held[7])==23,
            "private duplicate variables retain distinct inherited occurrences");
    require(held[0]==target.normal(),"literal and symbol normal dispatch agree");
    for(int i=0;i<sizeof(held);i++)
    {
        require(held[i]==fresh[i],sprintf("fresh inherited handle %d equality",i));
        if(i<3) require(held[i]==target.restored(saved[i]),sprintf("restored inherited handle %d equality",i));
        require(keys[held[i]]==i+1,"stored inherited mapping key resolves");
    }
    require(deep_eq(funcall(rebound),({target,this_object()})),"alien security binding differs from execution object");
    require(rebound!=target.identity_handle() && get_type_info(rebound,2)==target,
            "rebound handle retains security identity");
}
void replacement_rejected()
{
    if(catch(funcall(function void()
    {
        inspect_handles(2);
        msg("BLUEPRINT_NAMED_INHERITED: %d checks passed.\n",checks);
        rm("inherited_target.c");shutdown(0);
    });publish))shutdown(1);
}
void done(int failed)
{
    if(catch(funcall(function void()
    {
        require(!failed,"inherited binding metadata counted by GC");inspect_handles(2);
        require(deep_eq(m_indices(rebuilt),ordered),"fresh compacted inherited order remains stable");
        last_rt_warning=0; target.replace_me();
        require(last_rt_warning && strstr(last_rt_warning[0],"Cannot schedule")>=0,
                "normal closure still prevents replace_program scheduling");
        call_out(#'replacement_rejected,__ALARM_TIME__+1);
    });publish))shutdown(1);
}
void inspect()
{
    if(catch(funcall(function void()
    {
        mapping report=update_blueprint_result(request);
        require(report["status"]=="completed",sprintf("inherited update: %O",report["errors"]));
        inspect_handles(2);rebuilt=([]);for(int i=0;i<sizeof(held);i++)rebuilt[held[i]]=i+1;
        start_gc(#'done);
    });publish))shutdown(1);
}
void ready(int failed)
{
    require(!failed,"inherited baseline GC");ordered=m_indices(keys);inspect_handles(1);
    rm("inherited_target.c");write_file("inherited_target.c",body(2));
    request=update_blueprint("inherited_target");call_out(#'inspect,__ALARM_TIME__+1);
}
void run_test()
{
    call_out(#'shutdown,60,1);rm("inherited_target.c");write_file("inherited_target.c",body(1));
    source=load_object("inherited_target");target=clone_object(source);held=target.handles();saved=({});
    for(int i=0;i<6;i++) saved+=({target.saved(i)});
    rebound=bind_lambda(symbol_function("identity",target),this_object());
    keys=([]);for(int i=0;i<sizeof(held);i++)keys[held[i]]=i+1;
    start_gc(#'ready);
}
#else
void run_test(){shutdown(0);}
#endif
string *epilog(int eflag){if(catch(run_test();publish))shutdown(1);return 0;}
