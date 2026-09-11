#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/sys/configuration.h"
#include "/sys/rtlimits.h"
#if defined(__BLUEPRINT_UPDATE__)
object adapter, caller, stranger, base, source, first, second, other;
closure done;
int request, other_id, now = 2000000000, checks, phase;
string *notices = ({});
closure unchanged;
object **history;
int notice_count;
void next_mode();
int clock() { return now; }
void catch_tell(string text) { notices += ({text}); }
void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Coordinator workflow: " + why + "\n");
}
void finish(int failed)
{
    unchanged = 0;
    foreach (object ob : ({adapter,caller,stranger,base,first,second,source,other})) if (ob) destruct(ob);
    rm("operator_target.c"); rm("operator_other.c"); rm("requests-clock"); rm("requests-poll-budget");
    funcall(done, failed);
}
void inspect()
{
    mapping result;
    if (catch(
        result = caller.result(adapter, request),
        require(result["status"] == "completed" && result["updated"] == 1
             && result["blueprint_updated"] && result["matched"] == 1, "copied subset survives caller and policy mutation"),
        require(first.value() == 73 && first.version() == 2 && second.version() == 1, "selected identity and state preserved"),
        require(adapter.result(request)["id"] == request, "initiating wizard may read using current policy"),
        require(adapter.tick(), "one housekeeping closure remains"),
        require(sizeof(filter(notices, (: strstr($1, "completed") >= 0 :))) > 0, "clear completion message"); publish))
    { finish(1); return; }
    if (catch(
        adapter.policy(0,1,0,0,0),
        require(!!catch(caller.result(adapter, request)), "current result policy can revoke ownership access"),
        adapter.policy(0,2,0,0,0),
        require(!!catch(caller.result(adapter,request)), "zero throw cannot return an unauthorized empty result"),
        adapter.policy(0,0,0,0,0),
#ifdef __BLUEPRINT_UPDATE_TESTING__
        write_file("requests-poll-budget", "1\n", 1),
        require(caller.result(adapter, request)["status"] == "report-unavailable", "allocation/read failure is not expiry"),
        rm("requests-poll-budget"),
        require(caller.result(adapter, request)["status"] == "completed", "read error retains manual access"),
        write_file("requests-clock", "2000000299\n", 1),
        require(caller.result(adapter, request)["status"] == "completed", "driver TTL minus one"),
        write_file("requests-clock", "2000000300\n", 1),
        require(caller.result(adapter, request)["status"] == "unknown-expired", "specific driver expiry distinguished"),
        require(caller.result(adapter, request)["status"] == "unknown-expired", "forgotten local ID is explicit"),
        rm("requests-clock"),
#else
        require(caller.result(adapter, request)["status"] == "completed", "ordinary report remains manually readable"),
#endif
        require(caller.result(adapter, 0)["status"] == "unknown-expired", "unregistered local ID is explicit"); publish))
    { finish(1); return; }
    phase = 1;
    next_mode();
}
string generation(int version, int incompatible)
{
    string value = incompatible ? "string retained = \"bad\"; " : "int retained = 41; ";
    return "#pragma strong_types\n"
        + (version == 2 ? "int added = 17; " + value : value + (version >= 3 ? "int added = 17; " : ""))
        + "mixed value(){return retained;} "
        + (incompatible ? "void seed(int n){} " : "void seed(int n){retained=n;} ")
        + sprintf("int version(){return %d;} ", version)
        + "closure hold(){return function mixed(){return retained;};}\n";
}

void inspect_mode()
{
    mapping report = phase == 6 ? find_object("master").observe(request) : caller.result(adapter, request);
    if (catch(
        require(report["status"] == (phase == 5 ? "failed" : "completed"), sprintf("mode %d outcome %O", phase, report)),
        require(first.value() == 73 && second.value() == 84, "live values survive every coordinator mode"); publish))
    { finish(1); return; }
    if (phase == 1 || phase == 6 || phase == 7 || phase == 8)
    {
        if (catch(require(!report["matched"] && !report["blueprint_updated"] && report["selection"] == "explicit", "object-source explicit-empty mode"); publish))
        { finish(1); return; }
    }
    else if (phase == 2)
    {
        if (catch(require(report["updated"] == 1 && report["already_current"] == 1 && report["selection"] == "all"
                       && !report["blueprint_updated"], "omitted targets and already-current unsupported dependency"); publish))
        { finish(1); return; }
        unchanged = 0;
    }
    else if (phase == 3)
    {
        if (catch(require(!report["matched"] && report["blueprint_updated"] && first.version() == 2 && second.version() == 2,
                           "path-source explicit-empty updates only blueprint"); publish))
        { finish(1); return; }
    }
    else if (phase == 4)
    {
        if (catch(require(report["updated"] == 2 && report["blueprint_updated"] && report["selection"] == "all"
                       && first.version() == 3 && second.version() == 3, "explicit zero selects complete cohort with reordered slots"); publish))
        { finish(1); return; }
    }
    else if (phase == 5)
    {
        if (catch(require(blueprint_outcome(report) == "SCHEMA_INCOMPATIBLE" && !report["updated"]
                       && first.version() == 3, "coordinator reports compatibility failure without partial install"); publish))
        { finish(1); return; }
    }
    for (int i = 0; i < 3; i++)
    {
        object *after = clones(source, i);
        if (catch(require(!sizeof(after - history[i]) && !sizeof(history[i] - after), "historical clones selection is unchanged"); publish))
        { finish(1); return; }
    }
    if (catch(require(blueprint(first) == source && blueprint(second) == source, "blueprint follows currently installed program's blueprint"); publish))
    { finish(1); return; }
    if (++phase == 9)
    {
        msg("BLUEPRINT_COORDINATOR: %d checks, actual operator workflow, ownership, selections, polling, history and expiry passed.\n", checks);
        finish(0);
    }
    else next_mode();
}

void next_mode()
{
    if (catch(
        require(sizeof(filter(call_out_info(), (: $1[0] == adapter :))) <= 1, "at most one housekeeping callout"),
        require(!!catch(caller.submit(adapter, source, 7)), "nonzero integer selection rejected locally"); publish))
    { finish(1); return; }
    if (phase == 1)
    {
        history = ({clones(source,0), clones(source,1), clones(source,2)});
        request = caller.submit(adapter, source, ({}));
    }
    else if (phase == 2)
    {
        unchanged = first.hold();
        request = caller.submit(adapter, source);
    }
    else if (phase == 3)
    {
        write_file("operator_target.c", generation(3,0), 1);
        request = caller.submit(adapter, "operator_target", ({}));
    }
    else if (phase == 4) request = caller.submit(adapter, "operator_target", 0);
    else if (phase == 5)
    {
        write_file("operator_target.c", generation(4,1), 1);
        request = caller.submit(adapter, "operator_target", ({first,second,first}));
    }
    else if (phase == 6)
    {
        request = caller.submit(adapter, source, ({}));
        now += 600;
        if (catch(require(caller.result(adapter,request)["status"] == "unknown-expired"
                       && find_object("master").observe(request)["status"] == "pending", "local expiry does not cancel driver request"); publish))
        { finish(1); return; }
    }
    else if (phase == 7)
    {
        destruct(adapter); adapter = clone_object("adapter"); adapter.configure(caller,this_object());
        notice_count = sizeof(notices);
        request = limited((: caller.submit(adapter, source, ({})) :), LIMIT_CALLOUTS, 1);
        if (catch(
            require(request > 0 && caller.result(adapter,request)["status"] == "pending", "scheduling failure preserves admitted ID and manual access"),
            require(sizeof(filter(notices[notice_count..], (: strstr($1,"automatic polling unavailable") >= 0 :))) == 1, "scheduling failure message"); publish))
        { finish(1); return; }
    }
    else
    {
        request = caller.submit(adapter, source, ({}));
        adapter.policy(0,1,0,0,0); notice_count = sizeof(notices);
        if (catch(
            require(adapter.tick() && sizeof(notices) == notice_count, "internal polling honors current stored-pair policy"),
            require(!!catch(caller.result(adapter, request)), "revoked operator cannot read"); publish))
        { finish(1); return; }
        adapter.policy(0,0,0,0,0);
    }
    call_out(#'inspect_mode, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    done = callback;
    if (catch(
        require(file_size("coordinator_source.c") > 0, "reusable coordinator is shipped"),
        configure_object(this_object(), OC_COMMANDS_ENABLED, 1),
        base = clone_object("blueprint_update"), adapter = clone_object("adapter"),
        caller = clone_object("operator"), stranger = clone_object("operator"),
        caller.configure(this_object()), stranger.configure(this_object()),
        adapter.configure(caller, this_object()),
        write_file("operator_target.c", generation(1,0), 1),
        source = load_object("operator_target"), first = clone_object(source), second = clone_object(source),
        first.seed(73), second.seed(84),
        write_file("operator_other.c", "int value(){return 19;}\n", 1), other = load_object("operator_other"),
        require(!!catch(caller.submit(base, source, ({}))), "unadapted policy denies"),
        adapter.policy(1,0,0,0,0),
        require(!!catch(caller.submit(adapter, source, ({}))), "adapter denial does not consume slot"),
        adapter.policy(2,0,0,0,0),
        require(!!catch(caller.submit(adapter, source, ({}))), "zero throw from policy cannot return a fictional ID or consume capacity"),
        adapter.policy(0,0,1,0,second),
#ifdef __BLUEPRINT_UPDATE_TESTING__
        write_file("requests-clock", "2000000000\n", 1),
#endif
        write_file("operator_target.c", generation(2,0), 1); publish))
    { finish(1); return; }
    object *selection = ({first});
    if (catch(
        request = caller.submit(adapter, "operator_target", selection), selection[0] = second,
        require(caller.result(adapter, request)["status"] == "pending", "driver work remains deferred"),
        require(find_object("master").submitter() == adapter, "coordinator is actual driver owner"),
        require(!!catch(caller.direct_result(request)), "submitting command cannot bypass driver ownership"),
        require(!!catch(stranger.result(adapter, request)), "other caller cannot read stored owner pair"),
        require(adapter.tick(), "automatic poll closure installed"),
        require(caller.result(adapter, request)["status"] == "pending", "poll timeout does not cancel or fail request"),
        require(sizeof(filter(notices, (: strstr($1, "polling stopped") >= 0 :))) == 1, "bounded pending polling message"),
        adapter.policy(0,0,0,1,other),
        other_id = caller.submit(adapter, other, ({})),
        require(adapter.rejected(), "reserved final slot cannot be stolen by policy reentry"),
        require(!!catch(caller.submit(adapter, other, ({}))), "active local capacity rejects before admission"),
        adapter.policy(0,0,0,0,0),
        call_out(#'inspect, __ALARM_TIME__ + 1); publish)) finish(1);
}
#endif
