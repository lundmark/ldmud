#include "/inc/base.inc"
#include "/sys/configuration.h"

#ifdef __BLUEPRINT_UPDATE__
object source, target;
closure done;
int request, heartbeats, attempts;

void heart_beat() { heartbeats++; }

void poll()
{
    mapping report = update_blueprint_result(request);
    if (report["status"] == "pending" && attempts++ < 12)
    {
        call_out("poll", 1);
        return;
    }
    mixed error = catch(funcall(function void()
    {
        if (report["status"] != "completed" || report["updated"] != 1
         || target.version() != 2 || source.version() != 2 || heartbeats)
            raise_error("update must complete with heartbeats globally disabled\n");
        msg("BLUEPRINT_DISABLED_HEARTBEATS: publication completes before ordinary callout polling.\n");
    }); publish);
    configure_object(this_object(), OC_HEART_BEAT, 0);
    configure_driver(DC_ENABLE_HEART_BEATS, 1);
    destruct(target); destruct(source); rm("disabled_target.c");
    if (error) shutdown(1); else funcall(done);
}

void run(closure callback)
{
    done = callback;
    write_file("disabled_target.c", "int version(){return 1;}\n");
    source = load_object("disabled_target"); target = clone_object(source);
    rm("disabled_target.c");
    write_file("disabled_target.c", "int version(){return 2;}\n");
    configure_object(this_object(), OC_HEART_BEAT, 1);
    configure_driver(DC_ENABLE_HEART_BEATS, 0);
    request = update_blueprint("disabled_target");
    call_out("poll", __ALARM_TIME__ + 1);
}
#endif
