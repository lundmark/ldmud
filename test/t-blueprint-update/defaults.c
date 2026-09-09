#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/gc.inc"
#include "/inc/deep_eq.inc"

#ifdef __BLUEPRINT_UPDATE__
closure done;
object previous, blueprint;
int request, literal_request, checks;
string *unsupported;
string *aggregates;
int shared_case;

void run_composites();
void run_shared();

void require(int condition, string description)
{
    checks++;
    if (!condition)
        raise_error("Defaults: " + description + "\n");
}

void clean()
{
    if (previous) destruct(previous);
    if (blueprint) destruct(blueprint);
    rm("defaults_target.c");
}

void collected(int failed)
{
    mixed err = catch(
        require(!failed, "default report survives GC"),
        require(sizeof(update_blueprint_result(literal_request)["variable_changes"]) == 1,
                "default evidence survives program disposal"); publish);
    if (!err)
        msg("BLUEPRINT_DEFAULTS: %d checks passed.\n", checks);
    funcall(done, !!err);
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        mixed *changes = report["variable_changes"];
        string *names = ({"charges", "weight", "reference", "union_zero",
                         "explicit_zero", "negative", "fraction", "text", "octets"});
        mixed *expected = blueprint.values();

        require(report["status"] == "failed" && report["updated"] == 0,
                "default preparation does not migrate");
        require(sizeof(changes) == 1, "old generation described");
        require(sizeof(changes[0]["added"]) == sizeof(names), "all added declarations described");
        for (int index = 0; index < sizeof(names); index++)
        {
            string name = names[index];
            mixed *entries = filter(changes[0]["added"], (: $1["name"] == name :));
            mapping description;
            require(sizeof(entries) == 1, name + ": unique declaration");
            require(mappingp(entries[0]["default"]), name + ": default descriptor reported");
            description = entries[0]["default"];
            require(description["kind"] == (index >= 1 && index <= 3 ? "implicit" : "literal"),
                    name + ": syntax classification");
            require(description["value"] == expected[index], name + ": ordinary value parity");
            require(intp(description["value"]) == intp(expected[index])
                    && floatp(description["value"]) == floatp(expected[index])
                    && stringp(description["value"]) == stringp(expected[index])
                    && bytesp(description["value"]) == bytesp(expected[index]),
                    name + ": ordinary scalar type parity");
        }
        require(previous.version() == 1 && previous.retained_value() == 41,
                "old executable and retained value unchanged");
        clean();
        run_composites();
    }); publish);
    if (err)
    {
        clean();
        funcall(done, 1);
    }
}

void inspect_composites()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        mixed *added = report["variable_changes"][0]["added"];
        mixed *expected = blueprint.values();
        mixed *actual = ({});
        for (int i = 0; i < sizeof(unsupported); i++)
        {
            string name = "unsupported" + i;
            mixed *entries = filter(added, (: $1["name"] == name :));
            require(sizeof(entries) == 1 && entries[0]["default"]["kind"] == "unsupported",
                    unsupported[i] + ": unsupported syntax despite ordinary initialization");
        }
        for (int i = 0; i < sizeof(aggregates); i++)
        {
            string name = "aggregate" + i;
            mixed *entries = filter(added, (: $1["name"] == name :));
            mapping description = entries[0]["default"];
            require(description["kind"] == "literal", aggregates[i] + ": literal tree captured");
            actual += ({description["value"]});
            if (i != 8)
                require(deep_eq(actual[i], expected[i]), aggregates[i] + ": native value/type parity");
        }
        require(sizeof(actual[8]) == 5, "aggregate keys preserve native identity and empty-array sharing");
        require(widthof(actual[2]) == 1 && widthof(actual[3]) == 0
                && widthof(actual[4]) == 2 && widthof(actual[5]) == 3,
                "empty, zero, multivalue, and explicit mapping widths");
        actual[9][0][0][0] = 99;
        require(actual[9][1][0][0] == 1, "independent nested literal occurrences are fresh");
        require(previous.version() == 1, "composite preparation preserves old program");
        literal_request = request;
        clean();
        run_shared();
    }); publish);
    if (err)
    {
        clean();
        funcall(done, 1);
    }
}

void run_composites()
{
    string common = "#pragma init_variables\n"
                    "struct data { int member; };\n"
                    "int counter = 2;\n"
                    "mixed helper() { return 7; }\n"
                    "mixed retained = helper();\n";
    string source, values = "";
    unsupported = ({
        "helper()", "sizeof(\"x\")", "counter", "1+2", "\"a\"+\"b\"",
        "1-2", "2*3", "5/2", "5%2", "1|2", "1&2", "1^2",
        "1<<2", "4>>1", "4>>>1", "1==1", "1!=2", "1<2", "2<=2",
        "2>1", "2>=2", "1 ? 2 : 3", "0 || 1", "1 && 2", "!1", "~1",
        "(float)1", "({int})0", "++counter", "counter++", "counter=3",
        "#'helper", "function int() { return 1; }", "(: 1 :)", "catch(1)",
        "'symbol", "'({1})", "(<data>1)", "[int]", "decltype(1)",
        "({1})[0]", "({1,2})[0..0]", "([\"x\":1])[\"x\"]",
        "(0, 1)", "([ : 1+1 ])", "({helper()})", "([1:helper()])"
    });
    aggregates = ({
        "({1, ({2}), ([\"x\":({3})]),})", "({})", "([])", "([1,1,2])",
        "([\"a\":1;2,\"b\":3;4,])", "([ : (3) ])",
        "([\"a\":1;2,\"a\":3;4])", "([1:1,1.0:2,\"x\":3,b\"x\":4])",
        "([({}):1,({}):2,({1}):3,({1}):4,([]):5,([]):6])",
        "({({({1})}),({({1})})})",
        "([\"x\":({([1:({2})])});([2:({3})]),\"x\":({([3:({4})])});([4:({5})])])",
        "({\"a\\x00z\",b\"a\\x00z\",\"\\u00e5\\u00e4\\u00f6\"})",
        "({__INT_MIN__, -__INT_MIN__, -(-__INT_MIN__), __INT_MAX__, -0.0, 1.0e-300})"
    });
    clean();
    write_file("defaults_target.c", common + "int version() { return 1; }\n"
               "mixed *values() { return ({}); }\n");
    blueprint = load_object("defaults_target");
    previous = clone_object(blueprint);
    destruct(blueprint);
    source = common + "int version() { return 2; }\n";
    for (int i = 0; i < sizeof(unsupported); i++)
        source += sprintf("mixed unsupported%d = %s;\n", i, unsupported[i]);
    for (int i = 0; i < sizeof(aggregates); i++)
    {
        source += sprintf("mixed aggregate%d = %s;\n", i, aggregates[i]);
        values += sprintf("aggregate%d,", i);
    }
    source += "mixed *values() { return ({" + values + "}); }\n";
    rm("defaults_target.c");
    write_file("defaults_target.c", source);
    blueprint = load_object("defaults_target");
#if __EFUN_DEFINED__(swap)
    swap(blueprint);
#endif
    request = update_blueprint(blueprint, ({previous}));
    call_out(#'inspect_composites, __ALARM_TIME__ + 1);
}

void validation_done(int failed)
{
    if (failed)
        funcall(done, 1);
    else
        start_gc(#'collected);
}

void inspect_shared()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        require(report["updated"] == 0 && blueprint.version() == 2,
                "source default preparation leaves loaded blueprint unchanged");
        if (shared_case < 2)
        {
            mixed *generations = filter(report["variable_changes"],
                (: sizeof(filter($1["added"], (: $1["name"] == "shared_value" :))) :));
            require(sizeof(generations) == 1, "clone generation with shared addition is unique");
            mixed *added = generations[0]["added"];
            mixed *entries = filter(added, (: $1["name"] == "shared_value" :));
            require(sizeof(entries) == 1 && entries[0]["default"]["kind"] == "shared",
                    sprintf("shared case %d unsupported initializer uses loaded blueprint decision: %O", shared_case, entries));
            require(!member(entries[0]["default"], "value"),
                    "shared live value does not enter terminal report");
            require(!sizeof(generations[0]["blockers"]),
                    "retained blueprint initializer is not demanded");
            require(previous.version() == 1, "old clone remains executable");
        }
        else
        {
            require(sizeof(report["variable_changes"]) == 1
                    && report["variable_changes"][0]["blueprint"] && !report["matched"],
                    "empty target set retains complete implicit blueprint evidence");
            require(sizeof(report["variable_changes"][0]["blueprint_defaults"]) == 1,
                    "required blueprint addition is described even without clones");
            require(report["errors"][0]["code"] == (shared_case == 2
                        ? "IMPLEMENTATION_INCOMPLETE" : "SCHEMA_INCOMPATIBLE"),
                    "empty target set still validates needed blueprint defaults");
        }
        clean();
        if (++shared_case < 4)
            run_shared();
        else
            load_object("default_validation").run(#'validation_done);
    }); publish);
    if (err)
    {
        clean();
        funcall(done, 1);
    }
}

void run_shared()
{
    string helper = "#pragma share_variables\n"
                    "mixed helper() { return this_object(); }\n";
    clean();
    write_file("defaults_target.c", helper + "int version() { return 1; }\n");
    blueprint = load_object("defaults_target");
    previous = clone_object(blueprint);
    destruct(blueprint);
    rm("defaults_target.c");
    write_file("defaults_target.c", helper + "mixed shared_value = helper();\n"
               "int version() { return 2; }\n");
    blueprint = load_object("defaults_target");
    if (shared_case)
    {
        rm("defaults_target.c");
        write_file("defaults_target.c", helper + "mixed shared_value = helper();\n"
                   + (shared_case == 2 ? "mixed added = ({1, ([2:3])});\n" : "")
                   + (shared_case == 3 ? "mixed added = helper();\n" : "")
                   + "int version() { return 3; }\n");
    }
    request = update_blueprint(shared_case ? "defaults_target" : blueprint,
                               shared_case < 2 ? ({previous}) : ({}));
    call_out(#'inspect_shared, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    done = callback;
    clean();
    write_file("defaults_target.c",
        "#pragma init_variables\n"
        "int retained = 41;\n"
        "int version() { return 1; }\n"
        "int retained_value() { return retained; }\n"
        "mixed *values() { return ({}); }\n");
    blueprint = load_object("defaults_target");
    previous = clone_object(blueprint);
    destruct(blueprint);
    rm("defaults_target.c");
    write_file("defaults_target.c",
        "#pragma init_variables\n"
        "int retained = 99;\n"
        "int charges = 5;\n"
        "float weight;\n"
        "object reference;\n"
        "float|int union_zero;\n"
        "float explicit_zero = 0;\n"
        "int negative = -(-(-7));\n"
        "float fraction = -(1.25);\n"
        "string text = \"hello\";\n"
        "bytes octets = b\"hello\";\n"
        "int version() { return 2; }\n"
        "int retained_value() { return retained; }\n"
        "mixed *values() { return ({charges, weight, reference, union_zero,"
        "explicit_zero, negative, fraction, text, octets}); }\n");
    blueprint = load_object("defaults_target");
#if __EFUN_DEFINED__(swap)
    swap(blueprint);
#endif
    request = update_blueprint(blueprint, ({previous}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
#endif
