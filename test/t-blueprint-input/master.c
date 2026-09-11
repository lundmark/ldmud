#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/client.inc"

#ifdef __BLUEPRINT_UPDATE__
object source, target, sibling, client, server;
int request, phase, got_input, got_callout, checks;
closure callback;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Named input: " + label + "\n");
}

string body(int version, int removed)
{
    string input = removed ? "" : "void input(string line) { load_object(\"master\").received(line," + version + "); }\n";
    string callout = "void called() { load_object(\"master\").called(" + version + "); }\n";
    return "#pragma strong_types, save_types, init_variables\n"
        + (version == 1 ? "int value=41;\n" : "int added=7; int value=41;\n")
        + (version == 1 ? input + callout : callout + "int extra() {return 1;}\n" + input)
        + "int state() { return value; }\n"
        + "int version() { return " + version + "; }\n";
}

void finish()
{
    if (!got_input || !got_callout) return;
    require(interactive(client) && interactive(server), "unchanged real connection survives callback-target update");
    require(source.version()==2 && target.version()==2 && sibling.version()==2,
            "callback target cohort uses candidate programs");
    msg("BLUEPRINT_NAMED_INPUT: %d checks, actual input and callout delivered.\n",checks);
    rm("input_target.c"); shutdown(0);
}

void received(string line, int version)
{
    msg("INPUT line delivered\n");
    require(line=="retained input" && version==2 && previous_object()==target,
            "real input invokes retained callback through shifted named binding");
    got_input=1; finish();
}

void called(int version)
{
    msg("INPUT callout delivered\n");
    require(version==2 && previous_object()==target,
            "preexisting callout invokes new implementation at its retained deadline");
    got_callout=1; finish();
}

void send_line() { tell_object(this_object(),"retained input\n"); }
void begin();
void connected_client(object ob) { msg("INPUT client connected %O controller %O server %O\n", ob,this_object(),server); client=ob; if (server) begin(); }
void connected_server(object ob) { msg("INPUT server connected %O controller %O client %O\n", ob,this_object(),client); server=ob; if (client) begin(); }

void run_client() { load_object("master").connected_client(this_object()); }
void run_server()
{
    object controller=load_object("master");
    input_to(controller.input_handle());
    controller.connected_server(this_object());
}

closure input_handle() { return callback; }
void inspect()
{
    msg("INPUT inspect %d phase %d\n", request,phase);
    mapping report=update_blueprint_result(request);
    if (!phase)
    {
        require(blueprint_outcome(report)=="HANDLE_DECLARATION_REMOVED", "pending real input blocks removal");
        require(!report["updated"] && !report["blueprint_updated"]
                && source.version()==1 && target.version()==1 && sibling.version()==1,
                "pending-input rejection preserves entire cohort");
        phase=1;
        rm("input_target.c"); write_file("input_target.c",body(2,0));
        // Registered before submission; no post-update rescheduling.
        call_out(symbol_function("called",target),4*__ALARM_TIME__+2);
        request=update_blueprint("input_target");
        call_out(#'inspect,__ALARM_TIME__+1);
    }
    else
    {
        require(report["status"]=="completed",sprintf("named input permits migration: %O",report["errors"]));
        require(callback==symbol_function("input",target), "pending callback and fresh handle remain equal");
        client.send_line();
    }
}

void begin()
{
    msg("INPUT begin\n");
    rm("input_target.c"); write_file("input_target.c",body(2,1));
    request=update_blueprint("input_target");
    call_out(#'inspect,__ALARM_TIME__+1);
}

void run_test()
{
    call_out(#'shutdown,60,1);
    rm("input_target.c"); write_file("input_target.c",body(1,0));
    source=load_object("input_target"); target=clone_object(source); sibling=clone_object(source);
    callback=symbol_function("input",target);
    msg("INPUT connecting to %O\n", driver_info(DI_MUD_PORTS));
    connect_self("run_server","run_client");
}
#else
void run_test() { shutdown(0); }
#endif
string *epilog(int eflag) { if (catch(run_test(); publish)) shutdown(1); return 0; }
