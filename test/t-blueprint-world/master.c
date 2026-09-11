#define OWN_INAUGURATE_MASTER
#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/deep_eq.inc"
#include "/inc/gc.inc"

int checks, initializers, creates, resets;
object source, room;
object *guards, *items, *references;
string *names;
mixed *aliases, *handles, *retired, *history, *notifications;
int *health, *beats, *ids = ({});
int phase, request, attempts, initializers_before, creates_before, resets_before;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Blueprint world: " + why + "\n");
}

void inaugurate_master(int arg)
{
    set_driver_hook(H_LOAD_UIDS, unbound_lambda(({}), "uid"));
    set_driver_hook(H_CLONE_UIDS, unbound_lambda(({}), "uid"));
    set_driver_hook(H_NOTIFY_FAIL, "");
    set_driver_hook(H_CREATE_OB, "create");
    set_driver_hook(H_CREATE_CLONE, "create");
    set_driver_hook(H_RESET, "reset");
    set_driver_hook(H_MOVE_OBJECT0,
        unbound_lambda(({'item, 'dest}), ({#'set_environment, 'item, 'dest})));
}

int track_initializer() { return ++initializers; }
void track_create() { creates++; }
void track_reset() { resets++; }

void notice(object who, int tag, int version, int hp)
{
    notifications += ({({who, tag, version, hp})});
}

#ifdef __BLUEPRINT_UPDATE__
string npc_source(int version)
{
    return "#pragma strong_types, save_types, init_variables\n"
        + "#include \"/sys/configuration.h\"\n"
        + (version >= 3 ? "int armor = 17;\n" : "")
        + (version % 2 ? "int hp; mapping wounds; int beats;\n"
                       : "mapping wounds; int beats; int hp;\n")
        + (version < 4 ? "mixed obsolete;\n" : "")
        + (version == 5 ? "string patrol;\n" : "int patrol;\n")
        + "int init_token = master().track_initializer();\n"
        + (version >= 3 ? "int added_action() { return 17; }\n" : "")
        + sprintf("int version() { return %d; }\n", version)
        + "void create() { master().track_create(); }\n"
        + "void reset() { master().track_reset(); }\n"
        + "void seed(int n) { hp=n; wounds=([\"scratch\":n]);"
        + (version < 4 ? " obsolete=([\"loot\":n]);" : "")
        + " configure_object(this_object(),OC_HEART_BEAT,1); }\n"
        + "void stop() { configure_object(this_object(),OC_HEART_BEAT,0); }\n"
        + "int query_hp() { return hp; }\n"
        + "mapping injuries() { return wounds; }\n"
        + "mixed *aliases() { return ({&hp}); }\n"
        + "closure *handles() { return ({#'query_hp, #'hp}); }\n"
        + (version < 4 ? "mixed retired_value() { return obsolete; }\n"
                       : "mixed retired_value() { return 0; }\n")
        + "void heart_beat() { beats++; }\n"
        + "int heartbeat_count() { return beats; }\n"
        + "void report_status(int tag) { master().notice(this_object(),tag,version(),hp); }\n"
        + "void schedule(int tag) { call_out(\"report_status\",__ALARM_TIME__+2,tag); }\n";
}

void clean()
{
    aliases = handles = history = notifications = 0;
    references = 0;
    foreach (object ob: items ? items : ({})) if (ob) destruct(ob);
    foreach (object ob: guards ? guards : ({})) if (ob) { ob.stop(); destruct(ob); }
    if (source) destruct(source);
    if (room) destruct(room);
    rm("npc.c");
}

void finish(int failed)
{
    if (catch(
        require(!failed, "world disposal leaves valid GC roots"),
        require(update_blueprint_result(ids[<1])["status"] == "completed",
                "terminal report survives source and clone disposal"); publish))
    { shutdown(1); return; }
    for (int i=0; i<3; i++)
        require(retired[i]["loot"] == 100+i, "removed values outlive their original objects");
    msg("BLUEPRINT_WORLD: %d checks, ordinary NPC state, inventory, callbacks and generations passed.\n", checks);
    shutdown(0);
}

void submit_next();

void inspect()
{
    mapping report = update_blueprint_result(request);
    int *versions = phase == 1 ? ({3,1,3})
                    : phase == 4 ? ({6,6,6}) : ({4,4,4});
    require(report["status"] == (phase == 3 ? "failed" : "completed"),
            sprintf("phase %d result: %O", phase, report["errors"]));
    require(report["matched"] == (phase == 1 ? 2 : 3), "complete selected clone count");
    require(report["updated"] == (phase == 3 ? 0 : phase == 1 ? 2 : 3),
            "whole selected cohort publishes or remains unchanged");
    require(report["blueprint_updated"] == (phase != 3), "source blueprint participates atomically");
    require(initializers == initializers_before && creates == creates_before
            && resets == resets_before, "no initializer, constructor or reset replay");
    if (phase == 2)
    {
        int removed;
        foreach (mapping generation: report["variable_changes"])
            removed += sizeof(generation["removed"]);
        require(removed > 0, "completed migration describes removed declarations");
    }
    if (phase == 3)
        require(blueprint_outcome(report) == "SCHEMA_INCOMPATIBLE", "retained type change rejects before mutation");
    for (int mode=0; mode<3; mode++)
        require(deep_eq(clones(source, mode), history[mode]), "historical clone selection remains stable");
    require(sizeof(notifications) == 3, "every scheduled target callback ran exactly once");
    for (int i=0; i<3; i++)
    {
        object guard = guards[i];
        require(guard == references[i] && object_name(guard) == names[i], "external references and names remain identical");
        require(guard.version() == versions[i] && guard.query_hp() == health[i], "behavior changes preserve per-NPC hit points");
        require(guard.injuries()["scratch"] == 100+i, "per-NPC aggregate state survives");
        require(environment(guard) == room && environment(items[i]) == guard
                && member(all_inventory(guard), items[i]) >= 0, "room placement and carried items survive");
        require(guard.heartbeat_count() > beats[i], "existing heartbeat scheduling continues");
        require(funcall(handles[i][0]) == health[i] && funcall(handles[i][1]) == health[i], "retained named function and variable handles resolve current slots");
        require(deep_eq(handles[i], guard.handles()), "fresh named handles preserve equality");
        int seen;
        foreach (mixed *event: notifications)
            if (event[0] == guard)
            {
                require(event[1] == phase && event[2] == versions[i] && event[3] == health[i],
                        "string callout observes the installed program and live state");
                seen++;
            }
        require(seen == 1, "callback identity is unique");
        aliases[i][0] = --health[i];
        require(guard.query_hp() == health[i], "external protected alias still writes the live variable");
        require(retired[i]["loot"] == 100+i, "externally retained removed value remains usable");
    }
    if (phase == 4)
    {
        clean();
        start_gc(#'finish);
    }
    else submit_next();
}

void poll()
{
    if (catch(
        require(++attempts < 30, "finite update and callback deadline"),
        update_blueprint_result(request); publish)) { clean(); shutdown(1); return; }
    if (update_blueprint_result(request)["status"] == "pending" || sizeof(notifications) < 3)
    { call_out(#'poll, 1); return; }
    if (catch(inspect(); publish)) { clean(); shutdown(1); }
}

void submit_next()
{
    phase++;
    int version = phase == 1 ? 3 : phase == 2 ? 4 : phase == 3 ? 5 : 6;
    write_file("npc.c", npc_source(version), 1);
    initializers_before = initializers;
    creates_before = creates;
    resets_before = resets;
    notifications = ({});
    beats = ({});
    foreach (object guard: guards)
    {
        beats += ({guard.heartbeat_count()});
        guard.schedule(phase);
    }
    request = phase == 1 ? update_blueprint("npc", ({guards[0],guards[2]}))
                         : update_blueprint("npc");
    ids += ({request});
    require(update_blueprint_result(request)["status"] == "pending", "submission returns before execution");
    attempts = 0;
    call_out(#'poll, 1);
}

void run_test()
{
    write_file("npc.c", npc_source(1), 1);
    source = load_object("npc");
    guards = ({clone_object(source),clone_object(source)});
    destruct(source);
    write_file("npc.c", npc_source(2), 1);
    source = load_object("npc");
    guards += ({clone_object(source)});
    room = clone_object("thing");
    items = ({});
    references = guards + ({});
    names = ({});
    aliases = handles = retired = ({});
    health = ({});
    for (int i=0; i<3; i++)
    {
        object guard = guards[i];
        guard.seed(100+i);
        guard.reset();
        move_object(guard, room);
        object item = clone_object("thing");
        items += ({item});
        move_object(item, guard);
        names += ({object_name(guard)});
        aliases += ({guard.aliases()});
        handles += ({guard.handles()});
        retired += ({guard.retired_value()});
        health += ({100+i});
    }
    require(guards[0].version() == 1 && guards[2].version() == 2, "disposable world contains two historical blueprint generations");
    history = ({clones(source,0),clones(source,1),clones(source,2)});
    require(sizeof(history[2]) == 3, "all historical NPC clones are discoverable");
    submit_next();
}
#endif

string *epilog(int flag)
{
#ifdef __BLUEPRINT_UPDATE__
    call_out(#'shutdown, 90, 1);
    if (catch(run_test(); publish)) { clean(); shutdown(1); }
#else
    msg("BLUEPRINT_WORLD: feature disabled.\n");
    shutdown(0);
#endif
    return 0;
}
