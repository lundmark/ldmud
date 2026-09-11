#include "/inc/base.inc"

#ifdef __BLUEPRINT_UPDATE__
object source, first, second, unselected, item;
object *references;
mapping object_keys;
string *names;
int request, phase, attempts, checks, callbacks;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Transaction: " + label + "\n");
}

void callback(object who, int state)
{
    require(who == first && state == 73 && who.behavior_version() == 2,
            "scheduled string callback preserved across publication");
    callbacks++;
}

void clean()
{
    foreach (object ob: ({item, first, second, unselected, source}))
        if (ob) destruct(ob);
    references = 0;
    object_keys = 0;
    rm("target.c");
}

void run_case();

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
        require(first.behavior_version() == 2 && second.behavior_version() == 2,
                "the original selected objects execute version 2");
        require(report["status"] == "completed", sprintf("completed report: %O", report));
        require(report["updated"] == (phase ? 3 : 2) && report["blueprint_updated"] == 1,
                "whole selected cohort counted");
        require(source.behavior_version() == 2 && first.hp() == 37 && second.hp() == 61,
                "new behavior preserves distinct retained live values");
        require(first.charges() == 5 && second.charges() == 5,
                "added literal initialized in each clone");
        require(implode(map(({source,first,second,unselected}), #'object_name), ",") == implode(names, ","),
                "original object names preserved");
        require(references[0] == first && references[1] == second && member(references, first) == 0,
                "external object references preserved");
        require(object_keys[first] == "first" && object_keys[second] == "second",
                "external object mapping keys preserve identity");
        require(environment(item) == first && member(all_inventory(first), item) >= 0,
                "inventory preserved");
        require(unselected.behavior_version() == (phase ? 2 : 1), "selection respected");
        require(blueprint(source) == source && blueprint(first) == source
                && blueprint(second) == source && blueprint(unselected) == source,
                "current and retained old generations preserve the source association");
        object fresh=clone_object(unselected);
        require(fresh.behavior_version() == 2 && blueprint(fresh) == source,
                "cloning an old-generation object uses the current source blueprint");
        destruct(fresh);
        require(sizeof(report["variable_changes"][0]["removed"]) == 1,
                "removed obsolete declaration reported");
        require(find_call_out("finish") >= 0, "master scheduling preserved");
    }); publish);
    if (error) { clean(); shutdown(1); }
}

void finish()
{
    require(callbacks == phase + 1, "target string callback ran once");
    clean();
    if (!phase++) run_case();
    else
    {
        msg("BLUEPRINT_TRANSACTION: %d checks passed.\n", checks);
        load_object("sequence").run((: load_object("disabled_heartbeats").run((: load_object("trace").run(#'shutdown) :)) :));
    }
}

void run_case()
{
    clean();
    write_file("target.c", "#pragma init_variables\n"
        "int hitpoints; int obsolete; int callback_state;\n"
        "void seed(int value){hitpoints=value;} int hp(){return hitpoints;}\n"
        "int behavior_version(){return 1;}\n"
        "void scheduled(){master().callback(this_object(),callback_state);}\n"
        "void schedule(){callback_state=73;call_out(\"scheduled\",2*__ALARM_TIME__+3);}\n");
    source = load_object("target");
    first = clone_object(source); second = clone_object(source); unselected = clone_object(source);
    first.seed(37); second.seed(61);
    item = clone_object("/item"); set_environment(item, first);
    references = ({first,second});
    object_keys = ([first:"first",second:"second"]);
    names = map(({source,first,second,unselected}), #'object_name);
    first.schedule();
    rm("target.c");
    write_file("target.c", "#pragma init_variables\n"
        "int callback_state; int charges=5; int hitpoints;\n"
        "void seed(int value){hitpoints=value;} int hp(){return hitpoints;}\n"
        "int charges(){return charges;} int behavior_version(){return 2;}\n"
        "void scheduled(){master().callback(this_object(),callback_state);}\n");
    attempts = 0;
    request = phase ? update_blueprint("target") : update_blueprint("target", ({first,second}));
    call_out("poll", __ALARM_TIME__+1);
    call_out("finish", 3*__ALARM_TIME__+5);
}
#endif

string *epilog(int flag)
{
#ifdef __BLUEPRINT_UPDATE__
    run_case();
#else
    msg("BLUEPRINT_TRANSACTION: feature disabled.\n"); shutdown(0);
#endif
    return 0;
}
