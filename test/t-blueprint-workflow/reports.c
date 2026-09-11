#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/inc/gc.inc"
#include "/inc/deep_eq.inc"

#if defined(__BLUEPRINT_UPDATE__)
object source, left, right;
closure *raw, *named;
closure done;
int request, phase, checks;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Blueprint reports: " + why + "\n");
}

string body(int version)
{
    return "#pragma strong_types, init_variables\n"
        + (version ? "int added = 93;\n" : "int obsolete = 17;\n")
        + (version == 2 ? "string value = \"changed\";\n" : "int value = 41;\n")
        + sprintf("int created, reset_count, initialized = %d;\n", version ? 999 : 1)
        + "void create() { created++; } void reset() { reset_count++; }\n"
        + "int *lifecycle() { return ({created, reset_count, initialized}); }\n"
        + "mixed read_value() { return value; }\n"
        + (version ? "" : "int discarded() { return obsolete; }\n")
        + "closure variable_handle() { return " + (version ? "0" : "#'obsolete") + "; }\n"
        + "closure handle() { return function mixed() { return value; }; }\n";
}

void write_generation(int version)
{
    write_file("report_target.c", body(version), 1);
}

mapping *rows(mapping report, object ob, string code)
{
    return filter(report["errors"], (: $1["object"] == object_name(ob)
                                     && $1["code"] == code :));
}

int removed(mapping report)
{
    foreach (mapping generation : report["variable_changes"])
        foreach (mapping declaration : generation["removed"])
            if (declaration["name"] == "obsolete") return 1;
    return 0;
}

void finish(int failed)
{
    raw = named = 0;
    if (left) destruct(left);
    if (right) destruct(right);
    if (source) destruct(source);
    rm("report_target.c");
    funcall(done, failed);
}

void inspect()
{
    mapping result = update_blueprint_result(request);
    if (catch(
        require(result["matched"] == 2 && !result["destroyed"], "complete cohort counts"),
        require(removed(result), "removed variable evidence on both success and failure"); publish))
    { finish(1); return; }
    if (catch(
        require(phase == 2 ? result["status"] == "completed" : result["status"] == "failed", "expected outcome"),
        require(left.read_value() == 41 && right.read_value() == 41, "state retained across success and failure"),
        require(deep_eq(left.lifecycle(), ({0,0,1})) && deep_eq(right.lifecycle(), ({0,0,1})), "no lifecycle or initializer replay"); publish))
    { finish(1); return; }
    if (phase != 2)
    {
        if (catch(
            require(!result["updated"] && !result["blueprint_updated"], "whole cohort rollback"),
            require(blueprint_outcome(result) == (phase == 0 ? "LIVE_CLOSURE" : "HANDLE_DECLARATION_REMOVED"), "primary error remains compatible"),
            require(sizeof(rows(result, left, "HANDLE_DECLARATION_REMOVED")) == 2
                 && sizeof(rows(result, right, "HANDLE_DECLARATION_REMOVED")) == 2, "every named blocker identified"),
            require(sizeof(filter(rows(result, left, "HANDLE_DECLARATION_REMOVED"),
                           (: $1["name"] == "discarded" && $1["kind"] == "function" :))) == 1
                 && sizeof(filter(rows(result, right, "HANDLE_DECLARATION_REMOVED"),
                           (: $1["name"] == "obsolete" && $1["kind"] == "variable" :))) == 1, "public declaration identity"),
            require(rows(result, right, "HANDLE_DECLARATION_REMOVED")[0]["role"] == "clone"
                 && rows(result, right, "HANDLE_DECLARATION_REMOVED")[0]["old_generation"] > 0, "instance role and generation"); publish))
        { msg("REPORT_EVIDENCE: %O\n", result); finish(1); return; }
        if (!phase && catch(
            require(sizeof(rows(result, left, "LIVE_CLOSURE")) == 1
                 && sizeof(rows(result, right, "LIVE_CLOSURE")) == 1, "both raw blockers have exact object names"); publish))
        { finish(1); return; }
        result["errors"][0]["code"] = "tampered";
        result["variable_changes"][0]["removed"][0]["name"] = "tampered";
        if (catch(require(blueprint_outcome(update_blueprint_result(request)) != "tampered"
                       && removed(update_blueprint_result(request)), "nested report copies isolated"); publish))
        { finish(1); return; }
        if (!phase) raw = 0; else named = 0;
        phase++;
        request = update_blueprint("report_target", ({left, right}));
        call_out(#'inspect, __ALARM_TIME__ + 1);
    }
    else
    {
        if (catch(require(result["updated"] == 2 && result["blueprint_updated"], "successful whole cohort installation"); publish))
        { finish(1); return; }
        msg("BLUEPRINT_REPORTS: %d checks, all-instance diagnostics, copies and workflow passed.\n", checks);
        finish(0);
    }
}

void run(closure callback)
{
    done = callback;
    if (catch(
        write_generation(0), source = load_object("report_target"),
        left = clone_object(source), right = clone_object(source),
        raw = ({left.handle(), right.handle()}),
        named = ({symbol_function("discarded", left), symbol_function("discarded", right),
                  left.variable_handle(), right.variable_handle()}),
        write_generation(1), request = update_blueprint("report_target", ({left,right})),
        require(update_blueprint_result(request)["status"] == "pending", "submission is deferred"),
        call_out(#'inspect, __ALARM_TIME__ + 1); publish)) finish(1);
}
#endif
