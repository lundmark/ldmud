#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"
#include "/inc/deep_eq.inc"

#ifdef __BLUEPRINT_UPDATE__
object source, target, other;
closure *held, *ordered;
closure unsupported, alien;
mapping keys, rebuilt;
string saved;
int request, checks, phase;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Named handles: " + label + "\n");
}

string body(int version, int change)
{
    string a = change == 3 ? "string alpha() { return \"changed\"; }\n"
        : "int alpha() { return first + " + version + "; }\n";
    string b = change == 1 ? "" : "int beta() { return "
        + (change == 2 ? "83" : "second") + " + " + version + "; }\n";
    return "#pragma strong_types, save_types, init_variables\n"
        + (change == 2 ? "int first=41;\n" : version == 1 ? "int first=41, second=83;\n"
                       : version == 2 ? "int added=7, second=83, first=41;\n" : "int first=41, second=83, added=7;\n")
        + (version == 1 || version == 3 ? a + b : b + "int extra() { return 9; }\n" + a)
        + (version == 3 ? "int extra() { return 9; }\n" : "")
        + "closure *handles() { return ({#'alpha, " + (change == 1 ? "0" : "#'beta")
        + ", #'first, " + (change == 2 ? "0" : "#'second") + "}); }\n"
        + "string serialized() { return save_value(handles()); }\n"
        + "closure *revive(string data) { return restore_value(data); }\n"
        + "void replace_me() { replace_program(\"absent\"); }\n"
        + "object *identity() { return ({this_object(),previous_object()}); }\n"
        + "closure unsupported(int kind) { int capture=17; switch(kind) {\n"
        + "case 0: return function int() { return 17; };\n"
        + "case 1: return function int() { return capture; };\n"
        + "case 2: return lambda(0, ({#'alpha}));\n"
        + "case 3: return lambda(0, #'first);\n"
        + "case 4: return bind_lambda(unbound_lambda(0,17)); } return 0; }\n";
}

void inspect_handles(int version)
{
    closure *fresh = target.handles();
    closure *restored = target.revive(saved);
    require(funcall(held[0]) == 41 + version && funcall(held[1]) == 83 + version,
            "retained lfun calls resolve shifted function slots");
    require(funcall(held[2]) == 41 && funcall(held[3]) == 83,
            "retained variable closures resolve shifted variable slots");
    require(funcall(alien) == 41 + version,
            "unchanged unbound lambda dispatches through retained alien closure constant");
    require(other.read()[0] == held[0] && funcall(other.read()[0]) == 41 + version,
            "separate object's retained array uses current binding");
    for (int i=0; i<4; i++)
    {
        require(held[i] == fresh[i] && held[i] == restored[i], "fresh and restored equivalent handle equality");
        require(keys[held[i]] == i+1, "stored closure mapping key lookup");
        require(to_int(held[i]) == to_int(fresh[i]), "introspection reports current physical slot");
    }
    require(deep_eq(target.revive(target.serialized()), fresh), "save path resolves current declaration names");
    require(!ordered || deep_eq(m_indices(keys), ordered), "retained comparison ordering");
}

void next();
void after_gc(int failed)
{
    mixed err = catch(funcall(function void()
    {
        require(!failed, "named handle GC ownership");
        inspect_handles(phase == 10 ? 3 : 2);
        require(deep_eq(m_indices(rebuilt), ordered), "rebuilt and compacted keys retain semantic order");
        if (phase == 10)
        {
            require(held[4] == symbol_function("extra",target) && funcall(held[4]) == 9,
                    "handle first acquired after an update survives another reorder");
            msg("BLUEPRINT_NAMED_HANDLES: %d checks passed.\n", checks);
            rm("handle_target.c");
            shutdown(0);
        }
        else { phase++; next(); }
    }); publish);
    if (err) shutdown(1);
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        string outcome = phase == 0 || phase == 10 ? "completed"
                       : phase <= 2 ? "HANDLE_DECLARATION_REMOVED"
                       : phase == 3 ? "SCHEMA_INCOMPATIBLE" : "LIVE_CLOSURE";
        require(blueprint_outcome(report) == outcome,
                sprintf("phase %d expects %s: %O",phase,outcome,report["errors"]));
        inspect_handles(phase == 10 ? 3 : 2);
        if (phase && phase != 10)
        {
            require(!report["updated"] && !report["blueprint_updated"], "failure publishes no cohort prefix");
            require(source.alpha() == 43 && other.read()[1].alpha() == 43,
                    "all cohort programs and values survive blocked attempts");
            unsupported = 0;
            phase++; next();
        }
        else
        {
            rebuilt = ([]);
            foreach(closure key: held) rebuilt[key] = keys[key];
            start_gc(#'after_gc);
        }
    }); publish);
    if (err) shutdown(1);
}

void next()
{
    if (phase == 9)
    {
        held += ({symbol_function("extra",target)});
        keys[held[4]] = 5;
        /* Compact the expanded mapping before recording the new ordering. */
        start_gc(function void(int failed)
        {
            require(!failed,"new post-update binding compacts");
            ordered = m_indices(keys);
            phase++; next();
        });
        return;
    }
    if (phase >= 4 && phase <= 8) unsupported = target.unsupported(phase-4);
    rm("handle_target.c");
    write_file("handle_target.c", body(phase == 10 ? 3 : 2, phase <= 3 ? phase : 0));
    request = update_blueprint("handle_target");
    call_out(#'inspect,__ALARM_TIME__+1);
}

void before_update(int failed)
{
    require(!failed, "pre-update compact mapping");
    ordered = m_indices(keys);
    inspect_handles(1);
    next();
}
#endif

void run_test()
{
#ifdef __BLUEPRINT_UPDATE__
    call_out(#'shutdown,180,1);
    rm("handle_target.c");
    write_file("handle_target.c", body(1,0));
    source = load_object("handle_target");
    target = clone_object(source);
    other = clone_object("holder");
    held = target.handles();
    other.keep(({held[0], clone_object(source)}));
    saved = target.serialized();
    // compile_closure_call emits an alien FUNCALL of the closure constant.
    alien = bind_lambda(unbound_lambda(0, ({held[0]})));
    keys = ([]);
    for (int i=0; i<sizeof(held); i++) keys[held[i]] = i+1;
    inspect_handles(1);
    start_gc(#'before_update);
#else
    shutdown(0);
#endif
}

string *epilog(int eflag) { if (catch(run_test(); publish)) shutdown(1); return 0; }
