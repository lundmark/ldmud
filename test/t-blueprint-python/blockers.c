#include "common.inc"

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
int phase, released;
string *outcomes = ({"HANDLE_DECLARATION_REMOVED", "HANDLE_DECLARATION_REMOVED",
                    "SCHEMA_INCOMPATIBLE", "PYTHON_HANDLE", "PYTHON_HANDLE",
                    "PYTHON_HANDLE", "PYTHON_HANDLE", "PYTHON_HANDLE"});
closure native_function, native_variable;
mapping native_keys;

void next();
void check_unchanged()
{
    require(first.version() == 1 && second.version() == 1 && source.version() == 1,
            "failed request changes no cohort program");
    require(first.read_value(0) == 173 && second.read_value(0) == 184,
            "failed request changes no cohort value");
    require(funcall(native_function, 1) == 174 && funcall(native_variable) == 73,
            "retained native handles remain callable");
    require(native_keys[native_function] == 1 && native_keys[native_variable] == 2,
            "retained native mapping keys remain accessible");
    require(python_handles_check(first, 1), "retained Python keys and metadata survive rollback");
}

void inspect()
{
    mapping report = update_blueprint_result(request);
    if (catch(funcall(function void()
    {
        string expected = released ? "completed" : outcomes[phase];
        require(blueprint_outcome(report) == expected,
                sprintf("blocker %d expected %s: %O", phase, expected, report["errors"]));
        if (!released)
        {
            require(!report["updated"] && !report["blueprint_updated"], "no failed cohort prefix");
            check_unchanged();
            require(python_handles_blocker_check(first, phase), "held blocker remains intact");
            python_handles_unblock();
            // The same valid candidate succeeds once an iterator/generated
            // wrapper is released; removal cases drop only their missing key.
            released = 1;
            write_generation(2, phase < 2 ? phase + 1 : 0);
            request = update_blueprint("python_target", ({first, second}));
            call_out(#'inspect, __ALARM_TIME__ + 1);
        }
        else
        {
            require(report["updated"] == 2, "released blocker permits entire cohort");
            require(python_handles_check(first, 2), "mixed Python handles follow successful recovery");
            require(funcall(native_function, 0) == 273 && funcall(native_variable) == 73,
                    "native handles follow successful recovery");
            native_function = native_variable = 0; native_keys = 0;
            clean(); phase++; released = 0; next();
        }
    }); publish)) { native_function = native_variable = 0; native_keys = 0; finish(1); }
}

void next()
{
    if (phase == sizeof(outcomes))
    {
        msg("BLUEPRINT_PYTHON_BLOCKERS: %d checks, Python-only removal and mixed cohort rollback passed.\n", checks);
        funcall(done, 0); return;
    }
    if (catch(
        setup(),
        require(python_handles_hold(first, second, 0), "preupdate retained keys"),
        native_function = symbol_function("read_value", first),
        native_variable = first.variable_handle(),
        native_keys = ([native_function: 1, native_variable: 2]),
        require(python_handles_block(first, phase), "Python-only blocker acquired"),
        write_generation(2, phase < 3 ? phase + 1 : 0),
        request = update_blueprint("python_target", ({first, second})),
        call_out(#'inspect, __ALARM_TIME__ + 1); publish)) finish(1);
}

void run(closure callback) { done = callback; next(); }
#endif
