#include "/inc/base.inc"

#ifdef __BLUEPRINT_UPDATE__
object source, sequential, skipped;
closure done;
int request, phase, attempts;

void clean()
{
    foreach (object ob: ({sequential, skipped, source})) if (ob) destruct(ob);
    rm("sequence_target.c");
}

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
        if (report["status"] != "completed") raise_error("sequence migration failed\n");
        if (!phase++)
        {
            if (sequential.version() != 2 || skipped.version() != 1 || skipped.value() != 61)
                raise_error("sequence deletion changed the skipped clone\n");
            rm("sequence_target.c");
            write_file("sequence_target.c", "int hp=5;int version(){return 3;}int value(){return hp;}\n");
            request = update_blueprint("sequence_target", ({sequential, skipped}));
            attempts = 0;
            call_out("poll", __ALARM_TIME__ + 1);
            return;
        }
        if (sequential.version() != 3 || sequential.value() != 5
         || skipped.version() != 3 || skipped.value() != 61
         || report["updated"] != 2)
            raise_error("deleted/re-added declaration must be fresh only in the sequential clone\n");
        msg("BLUEPRINT_SEQUENCE: deleted/re-added state is fresh; skipped compatible state survives.\n");
        clean();
        funcall(done);
    }); publish);
    if (error) { clean(); shutdown(1); }
}

void run(closure callback)
{
    done = callback;
    clean();
    write_file("sequence_target.c", "int hp;void seed(int n){hp=n;}int value(){return hp;}int version(){return 1;}\n");
    source = load_object("sequence_target");
    sequential = clone_object(source); skipped = clone_object(source);
    sequential.seed(37); skipped.seed(61);
    rm("sequence_target.c");
    write_file("sequence_target.c", "int version(){return 2;}\n");
    request = update_blueprint("sequence_target", ({sequential}));
    call_out("poll", __ALARM_TIME__ + 1);
}
#endif
