#include "/inc/base.inc"
#include "/inc/gc.inc"

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
object source, first, second, owner, other, later;
int request, nested, later_id, phase, observations, checks, errors, attempts, boundary_seen;
mapping committed;

void require(int ok, string label)
{
    checks++;
    if (!ok) { msg("RETIREMENT FAILURE: %s\n", label); shutdown(1); raise_error(label + "\n"); }
}

void callback_error() { raise_error("expected retirement LPC exception\n"); }
void drain_trace() { for(int i=0;i<20000;i++) { int unused=i+1; } }
void context_nested(int fail)
{
    require(context_replace(), "nested Python entry keeps restored LW context alive");
    if(fail) raise_error("expected LW context error\n");
}
void context_done()
{
    msg("BLUEPRINT_PYTHON_CONTEXT: nested LW context survives success/error and releases exactly once.\n");
    shutdown(0);
}

void batch_boundary()
{
    if (!nested || boundary_seen) return;
    require(update_blueprint_result(later_id)["status"] == "completed" && later.version() == 2,
            "detached B completes before the first backend hook following A retirement");
    require(update_blueprint_result(nested)["status"] == "pending",
            "pending C remains pending at that same backend boundary");
    boundary_seen = 1;
}

void runtime_error(string message, string program, string objectname, int line,
                   mixed culprit, int caught)
{
    if (strstr(message, "expected retirement LPC exception") >= 0)
    {
        errors++;
        require(update_blueprint_result(request)["status"] == "completed",
                "runtime error hook observes immutable committed outcome");
    }
}

void observe(int kind)
{
    mapping report = update_blueprint_result(request);
    require(kind == phase, "last owner finalized during its own transaction");
    require(source.version() == 2 && first.version() == 2 && second.version() == 2,
            "first finalizer sees every new cohort program");
    require(first.value() == 37 && second.value() == 61,
            "finalizer sees every retained value");
    require(report["status"] == "completed" && report["updated"] == 2 && report["blueprint_updated"],
            "outcome frozen before the first retired value");
#ifdef __BLUEPRINT_UPDATE_TESTING__
    write_file("requests-clock", sprintf("%d\n", report["completed_at"] + 301));
    require(update_blueprint_result(request)["status"] == "completed",
            "busy committed report cannot expire during retirement");
#endif
    require(!!catch(update_blueprint(source, ({}))), "family reserved throughout retirement callbacks");
#ifdef __BLUEPRINT_UPDATE_TESTING__
    rm("requests-clock");
#endif
    if (!nested) nested = update_blueprint(other, ({}));
    require(update_blueprint_result(nested)["status"] == "pending", "finalizer submission waits for next tick");
    require(update_blueprint_result(later_id)["status"] == "pending", "detached B remains pending throughout A retirement");
    committed = report;
    if (owner) destruct(owner);
    require(update_blueprint_result(request)["status"] == "completed", "owner death preserves committed result");
    observations++;
    garbage_collection();
}

void clean()
{
    foreach(object ob: ({first,second,source,owner,other,later})) if (ob) destruct(ob);
    foreach(string name: ({"retire_target.c","other.c","later.c"})) rm(name);
}

void next_case();
void collected(int failed)
{
    require(!failed, "retired references and remainder roots survive collection");
    require(update_blueprint_result(request)["status"] == "completed", "terminal outcome survives GC and owner death");
    clean();
    if (++phase < 9) next_case();
    else { msg("BLUEPRINT_RETIREMENT: %d checks; %d LPC error hooks.\n", checks, errors); context_start(); }
}

void poll()
{
    if ((update_blueprint_result(request)["status"] == "pending" || !nested
          || update_blueprint_result(nested)["status"] == "pending") && attempts++ < 12)
    {
        call_out("poll", 1); return;
    }
    require(observations == 3, "all three final owners disposed exactly once");
    require(update_blueprint_result(request)["status"] == committed["status"], "committed result remains immutable");
    require(update_blueprint_result(nested)["status"] == "completed", "later unrelated request completes after callback errors");
    require(boundary_seen && update_blueprint_result(later_id)["status"] == "completed",
            "detached B survives native retirement GC and completes in the same batch");
    require(update_blueprint_result(nested)["completed_at"] >= committed["completed_at"],
            "pending C survives native retirement GC and runs next tick");
    require(!this_player() && this_object() == master(), "backend callback context restored");
    if (phase == 8) require(errors == 3, "each ordinary LPC error reached its runtime hook");
#if __EFUN_DEFINED__(last_instructions)
    require(pointerp(last_instructions(100, 0)) && pointerp(last_instructions(100, 1)),
            "both trace modes work after the final old program reference is gone");
    catch(raise_error("expected post-retirement trace\n"); publish);
#endif
    start_gc(#'collected);
}

void next_case()
{
    observations = nested = attempts = boundary_seen = 0;
    write_file("other.c", "int value(){return 19;}\n");
    other = load_object("other");
    write_file("later.c", "int version(){return 1;}\n");
    later=load_object("later");
    rm("later.c");write_file("later.c", "int version(){return 2;}\n");
    write_file("retire_target.c", "#pragma init_variables\n"
        "int preserved; mixed removed; int retained;\n"
        "void seed(int n){retained=n;} int value(){return retained;} int version(){return 1;}\n"
        "void hold(mixed value,int kind){\n"
        "if(kind==1) removed=({({value})});\n"
        "else if(kind==2) removed=([({value}):({value})]);\n"
        "else if(kind==3) { mixed cell=value; removed=({&cell}); }\n"
        "else if(kind==4) { lwobject remote=new_lwobject(\"remote\");remote.keep(value);removed=remote; }\n"
        "else if(kind==5) { lwobject remote=new_lwobject(\"remote\");removed=remote.suspended(value);call_coroutine(removed); }\n"
        "else if(kind==6) { lwobject remote=new_lwobject(\"remote\");removed=remote.captured(value); }\n"
        "else removed=value; }\n");
    source=load_object("retire_target");first=clone_object(source);second=clone_object(source);
    first.seed(37);second.seed(61);
    retirement_seed(source,phase);retirement_seed(first,phase);retirement_seed(second,phase);
    /* Overwrite counted lightweight-object instruction-history roots before
     * submission; each removed graph must own the final Python reference.
     */
    for(int i=0;i<20000;i++) { int unused=i+1; }
    source.version();first.version();second.version();
    rm("retire_target.c");
    write_file("retire_target.c", "#pragma init_variables\nint preserved;int added=5;int retained;\n"
        "int value(){return retained;}int version(){return 2;}\n");
    owner=clone_object("owner");
#ifdef __BLUEPRINT_UPDATE_TESTING__
    write_file("retirement-pressure","1\n");
    write_file("retirement-gc","2\n");
#endif
    request=owner.submit("retire_target",({first,second}));
    later_id=update_blueprint("later");
    call_out("poll",__ALARM_TIME__+1);
}
#endif

string *epilog(int flag)
{
#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
    call_out("timeout", 300);
    next_case();
#else
    msg("BLUEPRINT_RETIREMENT: Python/blueprint support unavailable; skipped.\n"); shutdown(0);
#endif
    return 0;
}
void timeout() { shutdown(1); }
