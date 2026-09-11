#include "/inc/base.inc"
#ifdef __BLUEPRINT_UPDATE__
object source,target;
closure done;
int submitted;
void queued()
{
    if(target.report()["status"]!="pending") raise_error("Heartbeat submission ran in the admitting tick.\n");
    submitted++;
}
void observed(object ob,int version)
{
    mapping result=ob.report();
    int failed=ob!=target || version!=2 || submitted!=1 || result["status"]!="completed"
                || result["updated"]!=1 || !result["blueprint_updated"] || ob.charges()!=5;
    if(!failed) msg("BLUEPRINT_SELF_HEARTBEAT: target admission defers; next heartbeat sees installed cohort.\n");
    destruct(target);destruct(source);rm("self_target.c");
    funcall(done,failed);
}
void run(closure callback)
{
    done=callback;
    rm("self_target.c");
    write_file("self_target.c","#include \"/sys/configuration.h\"\nint request;object controller;\n"
        "void setup(object ob){controller=ob;configure_object(this_object(),OC_HEART_BEAT,1);}\n"
        "mapping report(){return update_blueprint_result(request);}\n"
        "void heart_beat(){if(!request){request=update_blueprint(\"self_target\");controller.queued();}else controller.observed(this_object(),1);}\n");
    source=load_object("self_target");target=clone_object(source);target.setup(this_object());
    rm("self_target.c");
    write_file("self_target.c","#include \"/sys/configuration.h\"\nobject controller;int charges=5;int request;\n"
        "mapping report(){return update_blueprint_result(request);}int charges(){return charges;}\n"
        "void heart_beat(){configure_object(this_object(),OC_HEART_BEAT,0);controller.observed(this_object(),2);}\n");
}
#endif
