#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/deep_eq.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, previous;
int request, current, checks;
mixed *cases;
void next_case();

void require(int condition, string label)
{
    checks++;
    if (!condition)
        raise_error("Default validation: " + label + "\n");
}

void clean()
{
    if (previous) destruct(previous);
    if (blueprint) destruct(blueprint);
    rm("validation_target.c");
    rm("validation_location.h");
    rm("defaults-rtt");
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        if (sizeof(cases[current]) > 5)
        {
            require(blueprint_outcome(report) == (cases[current][5] == 1
                        ? "SCHEMA_INCOMPATIBLE" : "completed"),
                    cases[current][0] + ": selective nonfinite default demand");
            require(previous.version() == (cases[current][5] == 1 ? 1 : 2)
                    && blueprint.version() == (cases[current][5] == 1 ? 1 : 2)
                    && previous.value() == 0,
                    "nonfinite initializer is never evaluated for a retained slot");
        }
        else
        {
        mixed *added = report["variable_changes"][0]["added"];
        mapping description = added[0]["default"];
        require(description["kind"] == "literal", cases[current][0] + ": supported literal syntax");
        require(!!description["type_warning"] == cases[current][2],
                cases[current][0] + ": ordinary final RTT warning policy");
        require(!sizeof(report["variable_changes"][0]["blockers"]),
                cases[current][0] + ": compatible default decision");
        require(deep_eq(description["value"], blueprint.value()),
                cases[current][0] + ": native value parity");
        if (cases[current][0] == "included identifier")
            require(description["file"] == "validation_location.h" && description["line"] == 3,
                    sprintf("identifier location precedes initializer lookahead and include return: %O:%O", description["file"], description["line"]));
        require(report["status"] == "completed" && previous.version() == 2 && blueprint.version() == 2,
                "supported defaults install the loaded candidate");
        }
        clean();
        if (++current < sizeof(cases))
            next_case();
        else
        {
            msg("BLUEPRINT_DEFAULT_VALIDATION: %d checks passed.\n", checks);
            funcall(done, 0);
        }
    }); publish);
    if (err)
    {
        clean();
        funcall(done, 1);
    }
}

void next_case()
{
    clean();
    write_file("validation_target.c", (sizeof(cases[current]) > 5 && cases[current][5] == 2
                ? "float added = 0.0;\n" : "")
                + "int version() { return 1; }\nmixed value() { return 0; }\n");
    blueprint = load_object("validation_target");
    previous = clone_object(blueprint);
    if (sizeof(cases[current]) <= 5)
        destruct(blueprint);
    rm("validation_target.c");
    write_file("validation_location.h", "\nint\nadded\n=\n7;\n");
    write_file("validation_target.c", "#pragma init_variables, weak_types, save_types\n"
               + cases[current][1] + "\nint version() { return 2; }\nmixed value() { return added; }\n");
    if (sizeof(cases[current]) <= 5)
        blueprint = load_object("validation_target");
    else
        require(check_compile("validation_target")[0], "nonfinite literal remains legal compile-only source");
    write_file("defaults-rtt", sprintf("%d %d\n", cases[current][3], cases[current][4]));
    request = update_blueprint(sizeof(cases[current]) > 5 ? "validation_target" : blueprint, ({previous}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    mixed *result;
    done = callback;
    clean();
    write_file("validation_target.c", "mixed huge = ([ : __INT_MAX__ ]);\n");
    result = check_compile("validation_target");
    require(result[0], "huge legal mapping width still compiles without native allocation");
    clean();
    cases = ({
        ({"union retry", "#pragma warn_rtt_checks\nint*|string* added = ({1,2});", 0, 1, 1}),
        ({"RTT enabled during RHS", "#pragma no_rtt_checks\nint*|string* added =\n#pragma warn_rtt_checks\n({1,2});", 0, 1, 1}),
        ({"RTT disabled during RHS", "#pragma warn_rtt_checks\nint*|string* added =\n#pragma no_rtt_checks\n({1,2});\n#pragma warn_rtt_checks", 0, 0, 1}),
        ({"final warning pragma", "#pragma rtt_checks\nint*|string* added = ({1,2});\n#pragma warn_rtt_checks", 0, 1, 1}),
        ({"no final RTT warning", "#pragma no_rtt_checks\nint*|string* added = ({1,2});\n#pragma rtt_checks", 0, 0, 0}),
        ({"included identifier", "#include \"validation_location.h\"", 0, 0, 0}),
        ({"floating zero", "float added = 0.0;", 0, 0, 0}),
        ({"negated floating zero", "float added = -0.0;", 0, 0, 0}),
        ({"needed nonfinite", "float added = 1.0e309;", 0, 0, 0, 1}),
        ({"retained nonfinite", "float added = 1.0e309;", 0, 0, 0, 2})
    });
    next_case();
}
#endif
