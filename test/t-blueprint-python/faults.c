#define TARGET_PATH "fault_target"
#define OWN_BODY
string body(int version, int change);
#include "common.inc"

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
#define MAX_FAULT_POINTS 400
int point, admissions, executions;
#ifdef PYTHON_FAULT_POINTS
int *fault_points = PYTHON_FAULT_POINTS;
int fault_index;
#endif
closure native_function, native_variable;
mapping native_keys;

void advance_point()
{
#ifdef PYTHON_FAULT_POINTS
    if (++fault_index >= sizeof(fault_points))
        raise_error("Python targeted allocation sweep did not reach success.\n");
    point = fault_points[fault_index];
#else
    point++;
#endif
}

string body(int version, int change)
{
    return "#pragma strong_types, save_types, init_variables\n"
        + (version == 2 ? "int added=912; int leading(){return -123;}\n" : "")
        + "public nosave int value=41;\n"
        + sprintf("public nomask int read_value(int delta){return value+delta+%d;}\n", version * 100)
        + "void set_value(int next){value=next;}\n"
        + sprintf("int version(){return %d;}\n", version)
        + "closure variable_handle(){return #'value;}\n";
}

mixed include_file(string path, string from, int system)
{
    object probe = clone_object("binding_probe");
    mixed err = catch(python_handles_probe(probe));
    destruct(probe);
    if (err) raise_error(err);
    return 0;
}

void unchanged()
{
    require(first.version() == 1 && second.version() == 1 && source.version() == 1,
            "injected failure changes no cohort program");
    require(first.read_value(0) == 173 && second.read_value(0) == 184,
            "injected failure changes no cohort values");
    require(python_handles_check(first, 1), "Python key identity and resolution survive failed preparation");
    require(funcall(native_function, 0) == 173 && funcall(native_variable) == 73,
            "native handles survive failed preparation");
    require(native_keys[native_function] == 1 && native_keys[native_variable] == 2,
            "native keys survive failed preparation");
}

void finish_faults(int failed)
{
    native_function = native_variable = 0; native_keys = 0;
    rm("migration-pipeline"); rm("migration-pipeline-collected");
    rm("python_binding_hook.h"); rm("binding_probe.c");
    object probe = find_object("binding_probe");
    if (probe) destruct(probe);
    finish(failed);
}

void submit();
void inspect()
{
    if (catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        require(report["status"] != "pending", "fault attempt terminates at next tick");
        if (report["status"] == "failed")
        {
            require(!report["updated"] && !report["blueprint_updated"], "failed preparation publishes no prefix");
            require(read_file("migration-pipeline-collected") != 0,
                    "failed partial plans passed legal native GC checkpoint");
            unchanged(); executions++; advance_point(); submit();
            return;
        }
        require(report["status"] == "completed" && report["updated"] == 2, "final attempt updates whole cohort");
        require(python_handles_check(first, 2), "Python handles follow recovery after injected failures");
        require(funcall(native_function, 0) == 273 && funcall(native_variable) == 73,
                "native handles follow recovery after injected failures");
        require(admissions && executions && python_handles_probe_failures() == 2,
                "both Python binding allocation failures become MemoryError with normal cleanup");
        msg("BLUEPRINT_PYTHON_FAULTS: %d checks, %d admission failures, %d execution failures, 2 binding MemoryErrors, success at %d.\n",
            checks, admissions, executions, point);
        finish_faults(0);
    }); publish)) finish_faults(1);
}

void submit()
{
#ifdef PYTHON_FAULT_POINTS
    while (fault_index < sizeof(fault_points))
#else
    while (point < MAX_FAULT_POINTS)
#endif
    {
        rm("migration-pipeline"); rm("migration-pipeline-collected");
        write_file("migration-pipeline", sprintf("%d\n", point));
        request = 0;
        if (catch(request = update_blueprint(TARGET_PATH, ({first, second}))))
        {
            require(!request, "admission failure returns no request ID");
            unchanged(); admissions++; advance_point(); continue;
        }
        call_out(#'inspect, 1);
        return;
    }
    raise_error("Python allocation sweep did not reach success.\n");
}

void run(closure callback)
{
    done = callback;
#ifdef __BLUEPRINT_UPDATE_TESTING__
#ifdef PYTHON_FAULT_POINTS
    point = fault_points[0];
#endif
    if (catch(
        setup(), require(python_handles_hold(first, second, 1), "retained keys before fault sweep"),
        native_function = symbol_function("read_value", first),
        native_variable = first.variable_handle(),
        native_keys = ([native_function: 1, native_variable: 2]),
        rm("binding_probe.c"),
        write_file("binding_probe.c", "#pragma save_types\nint probe=19; int read(){return probe;}\n"),
        load_object("binding_probe"),
        rm("python_binding_hook.h"), write_file("python_binding_hook.h", "\n"),
        write_generation(2, 0), write_file(TARGET_PATH ".c", "#include \"python_binding_hook.h\"\n"),
        submit(); publish)) finish_faults(1);
#else
    msg("BLUEPRINT_PYTHON_FAULTS: test injection disabled.\n");
    funcall(done, 0);
#endif
}
#endif
