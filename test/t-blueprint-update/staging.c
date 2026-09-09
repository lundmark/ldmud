#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, old, requester;
int request, current, checks, hooks, retries;
mixed *cases;

void require(int condition, string description)
{
    checks++;
    if (!condition)
        raise_error("Staging: " + description + "\n");
}

void clean()
{
    if (blueprint) destruct(blueprint);
    if (old) destruct(old);
    if (requester) destruct(requester);
    if (find_object("staging_requester")) destruct(find_object("staging_requester"));
    if (find_object("staging_left")) destruct(find_object("staging_left"));
    if (find_object("staging_right")) destruct(find_object("staging_right"));
    if (find_object("staging_parent")) destruct(find_object("staging_parent"));
    rm("staging_target.c");
    rm("staging_parent.c");
    rm("staging_left.c");
    rm("staging_right.c");
    rm("staging_hook.h");
    rm("staging_requester.c");
    rm("staging_fault_diagnostic");
    rm("staging_fault_type_context");
    rm("staging_fault_prolog");
    rm("staging_fault_provisional");
    rm("staging_fault_adopted");
    rm("staging_fault_type_allocation");
    rm("staging_fault_source_name");
    rm("staging_fault_include_input");
    rm("staging_fault_pressure_diagnostic");
    rm("staging_fault_pressure_inherit");
    rm("staging_fault_preprocessor_string");
    rm("staging_fault_preprocessor_append");
    rm("staging_fault_hook_argument");
    rm("staging_fault_pressure_struct_init");
    rm("staging_fault_pressure_cleanup");
    rm("staging_fault_function_storage");
    rm("staging_fault_variable_storage");
    rm("staging_fault_type_storage");
    rm("staging_fault_argument_storage");
    rm("staging_fault_shadow_storage");
    rm("staging_fault_local_identifier");
    rm("staging_fault_local_debug");
    rm("staging_fault_struct_member");
    rm("staging_fault_inline_storage");
    rm("staging_fault_struct_name");
    rm("staging_fault_argument_stack");
    rm("staging_fault_inline_locals");
    rm("staging_fault_bytes_keyword");
    rm("staging_fault_setup_identifier");
    rm("staging_fault_call_prefix");
    rm("staging_fault_inline_header");
    rm("staging_fault_argument_index");
    rm("staging_fault_struct_literal");
    rm("staging_fault_struct_fill");
}

void finish(int failed)
{
    clean();
    funcall(done, failed);
}

void compiler_hook()
{
    string label = cases[current][0];
    hooks++;
    if (hooks > 1)
        return;
    if (member(({"private struct", "equivalent struct", "struct prototype",
                  "failed prototype", "hidden parent struct"}), label) >= 0)
    {
        require(old.converted_size() == 1, "published struct remains old during compiler hook");
        require(old.roundtrip() == 7, "runtime RTT checks see old struct during compiler hook");
        garbage_collection(__MASTER_OBJECT__ ".gc.log");
    }
    else if (label == "destroy source")
    {
        destruct(blueprint);
        require(!!catch(load_object("staging_target")), "ordinary nested load remains compiler busy");
    }
    else if (label == "destroy requester")
    {
        destruct(requester);
        require(!!catch(load_object("staging_requester")), "requester reload remains compiler busy");
    }
    else if (label == "throw hook")
        raise_error("intentional staging compiler hook failure\n");
}

void next_case();
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping result;
        mixed *spec = cases[current];
        if (spec[0] == "destroy requester")
        {
            require(!requester && hooks == 1, "requester loss cancels before another compiler hook");
            requester = load_object("staging_requester");
            require(!!catch(requester.result(request)), "reloaded requester cannot recover canceled report");
            require(old.version() == 1, "canceled request leaves old executable unchanged");
            clean();
            current++;
            next_case();
            return;
        }
        result = update_blueprint_result(request);
        if (result["status"] == "pending" && retries++ < 5)
        {
            call_out(#'inspect, __ALARM_TIME__ + 1);
            return;
        }
        require(result["status"] == (spec[3] == "completed" ? "completed" : "failed"),
                spec[0] + ": successful candidates install; failures remain atomic");
        if (spec[3] == "VALIDATION_FAILED")
            require(member(({"COMPILE_FAILED", "COMPILE_RESOURCE_FAILED", "COMPILE_CALLBACK_FAILED",
                            "SOURCE_INVALIDATED", "RESOURCE_FAILED", "PREPARATION_FAILED", "REPORT_ALLOCATION_FAILED"}),
                           blueprint_outcome(result)) >= 0
                    && sizeof(result["errors"][0]["message"]), spec[0] + ": precise terminal failure");
        else
            require(blueprint_outcome(result) == spec[3], spec[0] + ": terminal outcome");
        if (spec[3] == "completed" || spec[3] == "SCHEMA_INCOMPATIBLE")
        {
            require(result["candidate_generation"] > 0, spec[0] + ": candidate retained for schema comparison");
            require(sizeof(result["variable_changes"]) == 1, spec[0] + ": old generation described");
            if (spec[0] == "execution source")
                require(result["variable_changes"][0]["added"][0]["name"] == "execution_time",
                        "source content is read at execution");
            if (member(({"private struct", "struct prototype", "hidden parent struct"}), spec[0]) >= 0)
            {
                require(hooks > 0, "compiler hook executed");
                require(old.converted_size() == 1 && old.roundtrip() == 7,
                        "candidate disposal preserves published struct");
                require(member(map(result["variable_changes"][0]["blockers"], (: $1["code"] :)),
                               "STRUCT_LAYOUT_CHANGED") >= 0,
                        "candidate retains its private changed layout");
            }
            if (member(({"pinned parent", "removed parent source", "reloaded parent", "equivalent struct"}), spec[0]) >= 0)
                require(!sizeof(result["variable_changes"][0]["blockers"]),
                        "changed parent source does not replace pinned loaded program");
        }
        else
            require(!result["candidate_generation"] && !sizeof(result["variable_changes"]),
                    spec[0] + ": rejected preparation has no partial evidence");
        if (spec[0] == "failed prototype" || spec[0] == "equivalent struct")
        {
            require(hooks == 1, spec[0] + ": compiler hook executed");
            require(old.converted_size() == 1 && old.roundtrip() == 7,
                    spec[0] + ": disposal preserves published struct");
        }
        if (spec[0] == "destroy source")
        {
            require(hooks == 1, "source loss cancels before another compiler hook");
            blueprint = load_object("staging_target");
            require(blueprint.version() == 2, "source may reload after completed rejection");
            require(!update_blueprint_result(request)["candidate_generation"],
                    "reloaded source cannot replace rejected candidate identity");
        }
        require(old.version() == 1, spec[0] + ": old executable unchanged");
        clean();
        current++;
        next_case();
    }); publish);
    if (err) finish(1);
}

void gc_done(int failed)
{
    if (!failed)
        msg("BLUEPRINT_STAGING: %d checks passed.\n", checks);
    finish(failed);
}

void next_case()
{
    clean();
    if (current == sizeof(cases))
    {
        start_gc(#'gc_done);
        return;
    }
    hooks = retries = 0;
    if (cases[current][0] == "hidden parent struct")
        write_file("staging_parent.c", "private struct data { int parent_member; }; struct data parent_state;\n");
    else
        write_file("staging_parent.c", "int parent_value() { return 3; }\n");
    if (cases[current][0] == "unrelated loaded parent")
        load_object("staging_parent");
    if (cases[current][0] == "ambiguous parent")
    {
        write_file("staging_left.c", "inherit \"staging_parent\";\n");
        load_object("staging_left");
        destruct(find_object("staging_parent"));
        rm("staging_parent.c");
        write_file("staging_parent.c", "int parent_value() { return 4; }\n");
        write_file("staging_right.c", "inherit \"staging_parent\";\n");
        load_object("staging_right");
    }
    write_file("staging_hook.h", "\n");
    write_file("staging_target.c", cases[current][1]);
    blueprint = load_object("staging_target");
    old = clone_object(blueprint);
    if (cases[current][0] == "destroy requester")
    {
        write_file("staging_requester.c",
            "int submit(object target) { return update_blueprint(\"staging_target\", ({target})); }\n"
            "mapping result(int id) { return update_blueprint_result(id); }\n");
        requester = load_object("staging_requester");
        request = requester.submit(old);
    }
    else
        request = update_blueprint("staging_target", ({old}));
    if (cases[current][0] == "diagnostic allocation")
        write_file("staging_fault_diagnostic", "\n");
    if (cases[current][0] == "type context allocation")
        write_file("staging_fault_type_context", "\n");
    if (cases[current][0] == "compiler setup")
        write_file("staging_fault_prolog", "\n");
    if (cases[current][0] == "provisional program")
        write_file("staging_fault_provisional", "\n");
    if (cases[current][0] == "adopted program")
        write_file("staging_fault_adopted", "\n");
    if (strstr(cases[current][0], "interned ") == 0)
        write_file("staging_fault_type_allocation", "\n");
    if (cases[current][0] == "source filename")
        write_file("staging_fault_source_name", "\n");
    if (cases[current][0] == "include input")
        write_file("staging_fault_include_input", "\n");
    if (cases[current][0] == "pressure diagnostic")
        write_file("staging_fault_pressure_diagnostic", "\n");
    if (cases[current][0] == "pressure inherit")
        write_file("staging_fault_pressure_inherit", "\n");
    if (cases[current][0] == "preprocessor string")
        write_file("staging_fault_preprocessor_string", "\n");
    if (cases[current][0] == "preprocessor append")
        write_file("staging_fault_preprocessor_append", "\n");
    if (cases[current][0] == "hook argument")
        write_file("staging_fault_hook_argument", "\n");
    if (cases[current][0] == "pressure struct initializer"
     || cases[current][0] == "pressure prior argument"
     || cases[current][0] == "pressure call name"
     || cases[current][0] == "pressure nested locals"
     || cases[current][0] == "pressure nested contexts"
     || cases[current][0] == "pressure explicit context"
     || cases[current][0] == "pressure enclosing context")
        write_file("staging_fault_pressure_struct_init", "\n");
    if (cases[current][0] == "pressure cleanup")
        write_file("staging_fault_pressure_cleanup", "\n");
    if (cases[current][0] == "function storage")
        write_file("staging_fault_function_storage", "\n");
    if (cases[current][0] == "variable storage")
        write_file("staging_fault_variable_storage", "\n");
    if (cases[current][0] == "type storage")
        write_file("staging_fault_type_storage", "\n");
    if (cases[current][0] == "argument storage")
        write_file("staging_fault_argument_storage", "\n");
    if (cases[current][0] == "shadow storage")
        write_file("staging_fault_shadow_storage", "\n");
    if (cases[current][0] == "local identifier")
        write_file("staging_fault_local_identifier", "\n");
    if (cases[current][0] == "local debug")
        write_file("staging_fault_local_debug", "\n");
    if (cases[current][0] == "struct member")
        write_file("staging_fault_struct_member", "\n");
    if (cases[current][0] == "inline storage")
        write_file("staging_fault_inline_storage", "\n");
    if (cases[current][0] == "struct name")
        write_file("staging_fault_struct_name", "\n");
    if (cases[current][0] == "argument stack")
        write_file("staging_fault_argument_stack", "\n");
    if (cases[current][0] == "inline locals")
        write_file("staging_fault_inline_locals", "\n");
    if (cases[current][0] == "bytes keyword")
        write_file("staging_fault_bytes_keyword", "\n");
    if (cases[current][0] == "setup identifier")
        write_file("staging_fault_setup_identifier", "\n");
    if (cases[current][0] == "call prefix")
        write_file("staging_fault_call_prefix", "\n");
    if (cases[current][0] == "inline header")
        write_file("staging_fault_inline_header", "\n");
    if (cases[current][0] == "argument index")
        write_file("staging_fault_argument_index", "\n");
    if (cases[current][0] == "struct literal scratch")
        write_file("staging_fault_struct_literal", "\n");
    if (cases[current][0] == "struct prototype members")
        write_file("staging_fault_struct_fill", "\n");
    require((requester ? requester.result(request) : update_blueprint_result(request))["candidate_generation"] == 0,
            "pending candidate generation stays unknown");
    rm("staging_target.c");
    write_file("staging_target.c", cases[current][2]);
    if (cases[current][0] == "pinned parent")
    {
        rm("staging_parent.c");
        write_file("staging_parent.c", "This parent source no longer compiles.\n");
    }
    if (cases[current][0] == "removed parent source")
        rm("staging_parent.c");
    if (cases[current][0] == "reloaded parent")
    {
        destruct(find_object("staging_parent"));
        rm("staging_parent.c");
        write_file("staging_parent.c", "int parent_value() { return 4; }\n");
        load_object("staging_parent");
    }
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
#ifndef __BLUEPRINT_UPDATE_TESTING__
    if (file_size("staging-faults") >= 0)
    {
        msg("BLUEPRINT_INSTRUMENTED: compiler fault sweep requires test build; skipped.\n");
        funcall(callback, 0);
        return;
    }
#endif
    string simple = "int version() { return 1; }\n";
    string structure = "#pragma strong_types, rtt_checks\n"
        "struct data { int first; }; struct data value;\n"
        "int version() { return 1; }\n"
        "int checked(struct data arg) { return arg.first; }\n"
        "int roundtrip() { return checked((<data> 7)); }\n"
        "int converted_size() { return sizeof(to_type(({7}), [struct data])); }\n";
    string changed_structure = "#pragma strong_types, rtt_checks\n"
        "struct data; struct data { int first; string second; }; struct data value;\n"
        "#include \"staging_hook.h\"\n"
        "int version() { return 2; }\n"
        "string nested(struct data** values) { return values[0][0].second; }\n"
        "struct data|string choice(int n) { return n ? (<data> 1, \"new\") : \"other\"; }\n";
    done = callback;
    cases = ({
        ({"execution source", simple, "int execution_time;\n" + simple, "completed"}),
        ({"failed compilation", simple, "This is not valid LPC.\n", "VALIDATION_FAILED"}),
        ({"failed struct initializer", simple,
            "struct new_data { string first; string second; };\n"
            "mixed value() { return (<new_data> first: \"one\", second: ); }\n" + simple,
            "VALIDATION_FAILED"}),
        ({"missing parent", simple, "inherit \"staging_absent\";\n" + simple, "VALIDATION_FAILED"}),
        ({"pinned parent", "inherit \"staging_parent\";\n" + simple,
            "inherit \"staging_parent\";\n" + simple, "completed"}),
        ({"removed parent source", "inherit \"staging_parent\";\n" + simple,
            "inherit \"staging_parent\";\n" + simple, "completed"}),
        ({"reloaded parent", "inherit \"staging_parent\";\n" + simple,
            "inherit \"staging_parent\";\n" + simple, "completed"}),
        ({"unrelated loaded parent", simple, "inherit \"staging_parent\";\n" + simple, "VALIDATION_FAILED"}),
        ({"ambiguous parent", "inherit \"staging_left\"; inherit \"staging_right\";\n" + simple,
            "inherit \"staging_parent\";\n" + simple, "VALIDATION_FAILED"}),
        ({"equivalent struct", structure, structure + "#include \"staging_hook.h\"\n", "completed"}),
        ({"struct prototype", structure, changed_structure, "SCHEMA_INCOMPATIBLE"}),
        ({"failed prototype", structure, changed_structure + "struct never_defined;\n", "VALIDATION_FAILED"}),
        ({"hidden parent struct", "inherit \"staging_parent\";\n" + structure,
            "inherit \"staging_parent\";\n" + changed_structure, "SCHEMA_INCOMPATIBLE"}),
        ({"private struct", structure,
            "#pragma strong_types, rtt_checks\n"
            "struct data { int first; string second; }; struct data value;\n"
            "#include \"staging_hook.h\"\n"
            "int version() { return 2; }\n"
            "string candidate_member(struct data arg) { return arg.second; }\n",
            "SCHEMA_INCOMPATIBLE"}),
        ({"destroy source", simple,
            "#include \"staging_hook.h\"\n#include \"staging_hook.h\"\nint version() { return 2; }\n",
            "VALIDATION_FAILED"}),
        ({"destroy requester", simple,
            "#include \"staging_hook.h\"\n#include \"staging_hook.h\"\n" + simple, "VALIDATION_FAILED"}),
        ({"throw hook", simple, "#include \"staging_hook.h\"\n" + simple, "VALIDATION_FAILED"}),
        ({"recovery", simple, simple, "completed"})
    });
    if (file_size("staging-faults") >= 0)
    {
        string saved_arguments = "#pragma strong_types, save_types\n";
        string *argument_names = ({});

        /* 65 * 17 saved types exceed the initial 2048-byte index area. */
        for (int arg = 0; arg < 17; arg++)
            argument_names += ({sprintf("int argument%d", arg)});
        for (int fn = 0; fn < 65; fn++)
            saved_arguments += sprintf("int saved%d(%s) { return 1; }\n",
                                       fn, implode(argument_names, ","));
        cases = ({
            ({"argument index", simple, saved_arguments + simple, "VALIDATION_FAILED"}),
            ({"struct literal scratch", simple,
                "struct new_data {string first;}; mixed value() { return (<new_data> first: \"one\"); }\n" + simple, "VALIDATION_FAILED"}),
            ({"struct prototype members", structure,
                "struct base_data {string first;}; struct data(base_data) {int second;};\n" + simple, "VALIDATION_FAILED"}),
            ({"function storage", simple, "struct data {int first;}; struct data make() {return (<data> 1);}\n" + simple, "VALIDATION_FAILED"}),
            ({"variable storage", simple, "struct data {int first;}; struct data value;\n" + simple, "VALIDATION_FAILED"}),
            ({"type storage", simple, "struct data {int first;}; mixed value() {return [struct data];}\n" + simple, "VALIDATION_FAILED"}),
            ({"argument storage", simple, "#pragma strong_types, save_types\nstruct data {int first;}; int take(struct data value) {return 1;}\n" + simple, "VALIDATION_FAILED"}),
            ({"shadow storage", simple, "int sizeof() {return 1;}\n" + simple, "VALIDATION_FAILED"}),
            ({"local identifier", simple, "int version() {int sizeof = 1; return sizeof;}\n", "VALIDATION_FAILED"}),
            ({"local debug", simple, "#pragma save_local_names\nint version() {string value = \"one\"; return 1;}\n", "VALIDATION_FAILED"}),
            ({"struct member", simple, "struct data {int first;};\n" + simple, "VALIDATION_FAILED"}),
            ({"inline storage", simple, "struct new_data {string first;}; mixed value() {return function struct new_data() { return (<new_data> \"one\"); };}\n" + simple, "VALIDATION_FAILED"}),
            ({"struct name", simple, "struct data {int first;};\n" + simple, "VALIDATION_FAILED"}),
            ({"argument stack", simple, "struct new_data {string first;}; mixed value() {return ({ (<new_data> \"one\"), (<new_data> \"two\") });}\n" + simple,
                "VALIDATION_FAILED"}),
            ({"inline locals", simple, "mixed value() {return function int() { return 1; };}\n" + simple, "VALIDATION_FAILED"}),
            ({"bytes keyword", simple, "#pragma no_bytes_type\n#define bytes custom\n#pragma bytes_type\n" + simple, "VALIDATION_FAILED"}),
            ({"setup identifier", simple, simple, "VALIDATION_FAILED"}),
            ({"call prefix", simple,
                "object \"staging_call_type\" target; mixed value() { return target->invoke(); }\n" + simple, "VALIDATION_FAILED"}),
            ({"inline header", simple,
                "struct new_data {string first;}; mixed value() { return function struct new_data(struct new_data argument) { return argument; }; }\n" + simple, "VALIDATION_FAILED"}),
            ({"pressure diagnostic", simple,
                "#pragma warn_dead_code\nint version() { return 1; string unreachable = \"value\"; }\n", "VALIDATION_FAILED"}),
            ({"pressure inherit", "inherit \"staging_parent\";\n" + simple,
                "inherit \"staging_parent\";\n" + simple, "VALIDATION_FAILED"}),
            ({"preprocessor string", simple,
                "#if \"left\" + (\"right\" + \"last\")\n#endif\n" + simple, "VALIDATION_FAILED"}),
            ({"preprocessor append", simple,
                "#if \"left\" + (\"right\" + \"last\")\n#endif\n" + simple, "VALIDATION_FAILED"}),
            ({"hook argument", simple,
                "#include \"staging_hook.h\"\n" + simple, "VALIDATION_FAILED"}),
            ({"pressure struct initializer", simple,
                "struct new_data { string first; string second; };\n"
                "mixed value() { return (<new_data> first: \"one\", second: \"two\"); }\n" + simple,
                "VALIDATION_FAILED"}),
            ({"pressure prior argument", simple,
                "struct new_data {string first;}; struct new_data prior;\n"
                "mixed value() { return ({ prior, (<new_data> \"one\") }); }\n" + simple,
                "VALIDATION_FAILED"}),
            ({"pressure call name", simple,
                "struct new_data { string first; string second; };\n"
                "mixed value() { return this_object()->accept((<new_data> first: \"one\", second: \"two\")); }\n" + simple,
                "VALIDATION_FAILED"}),
            ({"pressure nested locals", simple,
                "struct new_data {string first;};\n"
                "mixed value() { struct new_data outer; return function mixed() { struct new_data inner; return (<new_data> \"one\"); }; }\n" + simple,
                "VALIDATION_FAILED"}),
            ({"pressure nested contexts", simple,
                "struct new_data {string first;};\n"
                "mixed value() { struct new_data outer; return function mixed() { struct new_data middle; return function mixed() { return outer.first + (<new_data> \"one\").first; }; }; }\n" + simple,
                "VALIDATION_FAILED"}),
            ({"pressure explicit context", simple,
                "struct new_data {string first;};\n"
                "mixed value() { struct new_data outer; return function mixed(struct new_data argument) : struct new_data context = outer, struct new_data other = (<new_data> \"one\") { return context; }; }\n" + simple,
                "VALIDATION_FAILED"}),
            ({"pressure enclosing context", simple,
                "struct new_data {string first;};\n"
                "mixed value() { struct new_data outer; return function mixed(struct new_data argument) : closure nested = function mixed(struct new_data inner) : struct new_data context = outer, struct new_data other = (<new_data> \"one\") { return context; } { return nested; }; }\n" + simple,
                "VALIDATION_FAILED"}),
            ({"pressure cleanup", simple, simple, "VALIDATION_FAILED"}),
            ({"source filename", simple, simple, "VALIDATION_FAILED"}),
            ({"include input", simple,
                "#include \"staging_hook.h\"\n" + simple, "VALIDATION_FAILED"}),
            ({"interned type allocation", simple,
                "struct new_data { int first; };\n" + simple, "VALIDATION_FAILED"}),
            ({"interned array allocation", simple,
                "int ***retained;\n" + simple, "VALIDATION_FAILED"}),
            ({"interned union allocation", simple,
                "int|mapping retained;\n" + simple, "VALIDATION_FAILED"}),
            ({"interned object allocation", simple,
                "object \"staging_type_origin\" retained;\n" + simple, "VALIDATION_FAILED"}),
            ({"compiler setup", simple, simple, "VALIDATION_FAILED"}),
            ({"provisional program", simple,
                "struct data { int first; };\nstring retained;\n" + simple, "VALIDATION_FAILED"}),
            ({"adopted program", simple, simple, "VALIDATION_FAILED"}),
            ({"diagnostic allocation", simple,
                "#pragma warn_dead_code\nint version() { return 1; string unreachable = \"value\"; }\n", "VALIDATION_FAILED"}),
            ({"type context allocation", simple,
                "struct data { int first; };\n" + simple, "VALIDATION_FAILED"}),
            ({"recovery", simple, simple, "completed"})
        });
    }
    if (file_size("staging-fault-case") >= 0)
    {
        string selection = read_file("staging-fault-case");
        cases = filter(cases, (: $1[0] == selection || $1[0] == "recovery" :));
    }
    next_case();
}
#endif
