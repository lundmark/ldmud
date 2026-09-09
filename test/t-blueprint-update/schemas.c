#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/gc.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object *old_clones = ({});
object candidate;
int request, current, checks;
mixed *cases;
void require(int condition, string description)
{
    checks++;
    if (!condition) raise_error("Schema: " + description + "\n");
}
void clean()
{
    foreach (object ob: old_clones) if (ob) destruct(ob);
    old_clones = ({});
    if (candidate) destruct(candidate);
    rm("schema_target.c");
}
void next_case();
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping result = update_blueprint_result(request);
        mixed *changes = result["variable_changes"];
        mixed *spec = cases[current];
        require(result["status"] == "failed" && result["updated"] == 0, "diagnostics do not migrate");
        if (spec[0] == "budget boundary")
        {
            require((!sizeof(changes) && result["errors"][0]["code"] == "REPORT_SIZE_LIMIT"
                      && result["matched"] == 2
                      && strstr(result["errors"][0]["message"], "incomplete") >= 0)
                    || sizeof(changes) == 2
                    || (sizeof(changes) == 1
                        && member(map(changes[0]["blockers"], (: $1["code"] :)),
                                  "SCHEMA_MEMORY_LIMIT") >= 0),
                    "budget exhaustion never silently omits an old generation");
            clean(); current++; next_case();
            return;
        }
        require(sizeof(changes) == sizeof(spec[1]), spec[0] + ": all old generations described");
        require(result["candidate_generation"] > 0, "stable candidate generation label");
        foreach (mapping change: changes)
        {
            mixed *expected_matches = spec[3];
            if (spec[0] == "direct reintroduction")
                expected_matches = spec[3][member(changes, change)];
            require(change["old_generation"] > 0 && change["old_generation"] < result["candidate_generation"],
                    "distinct monotonic generations");
            if (spec[0] == "parent generation")
            {
                mapping edge = change["blockers"][0];
                require(edge["inherit_index"] == 0 && edge["old_parent"] == "/base"
                        && edge["candidate_parent"] == "/base"
                        && edge["old_parent_generation"] < edge["candidate_parent_generation"],
                        "changed parent generation has concrete edge evidence");
            }
            if (spec[0] == "direct reintroduction")
                require(sizeof(change["added"]) == member(changes, change),
                        "reintroduction preserves only a currently present declaration");
            if (spec[0] == "function reorder")
            {
                foreach (mapping fun: change["functions"])
                    require(fun["program"] == "/schema_target" && fun["occurrence"] == "$"
                            && !fun["generated"]
                            && ((fun["name"] == "f" && fun["old_slot"] == 0 && fun["new_slot"] == 1)
                             || (fun["name"] == "g" && fun["old_slot"] == 1 && fun["new_slot"] == 0)),
                            "named function declarations map after reorder");
                require(sizeof(change["functions"]) == 2, "both functions described");
            }
            if (spec[0] == "inline marker")
                require(sizeof(filter(change["functions"], (: $1["generated"] :))) == 1,
                        "inline without context is marked generated");
            if (spec[4])
                require(member(map(change["blockers"], (: $1["code"] :)), spec[4]) >= 0,
                        spec[0] + ": explicit compatibility blocker");
            else
            {
                require(!sizeof(change["blockers"]), spec[0] + sprintf(": compatible declarations %O", change["blockers"]));
                require(sizeof(change["matched"]) == sizeof(expected_matches), spec[0] + ": matched count");
                foreach (mixed *expected: expected_matches)
                {
                    mixed *matches = filter(change["matched"],
                        function int(mapping item) { return item["program"] == expected[0] && item["occurrence"] == expected[1]
                           && item["name"] == expected[2] && item["old_slot"] == expected[3]
                           && item["new_slot"] == expected[4]; });
                    require(sizeof(matches) == 1, spec[0] + ": declaration identity " + expected[2]);
                }
            }
        }
        if (spec[0] == "own reorder/add/remove")
        {
            require(changes[0]["added"][0]["name"] == "added" && changes[0]["removed"][0]["name"] == "removed",
                    "add and remove named");
            changes[0]["matched"][0]["name"] = "tampered";
            require(update_blueprint_result(request)["variable_changes"][0]["matched"][0]["name"] != "tampered",
                    "nested schema result isolation");
        }
        clean(); current++; next_case();
    }); publish);
    if (err) { clean(); funcall(done, 1); }
}
void schema_gc_done(int failed)
{
    mixed err = catch(
        require(!failed, "schema summaries survive terminal GC"),
        require(sizeof(update_blueprint_result(request)["variable_changes"]) == 1,
                "summaries outlive retired programs"); publish);
    if (!err)
        msg("BLUEPRINT_SCHEMA: %d checks passed.\n", checks);
    funcall(done, !!err);
}

void next_case()
{
    if (current == sizeof(cases))
    {
        start_gc(#'schema_gc_done);
        return;
    }
    foreach (string source: cases[current][1])
    {
        rm("schema_target.c"); write_file("schema_target.c", source);
        candidate = load_object("schema_target");
        old_clones += ({clone_object(candidate)}); destruct(candidate);
    }
    if (cases[current][0] == "parent generation")
    {
        destruct(find_object("base"));
        load_object("base");
    }
    rm("schema_target.c"); write_file("schema_target.c", cases[current][2]);
    candidate = load_object("schema_target");
#if __EFUN_DEFINED__(swap)
    swap(candidate);
#endif
    request = update_blueprint(candidate, old_clones);
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
void run(closure callback)
{
    string boundary = "";
    string traversal = "";
    done = callback;
    /* With the default 64 MiB report budget these declarations fill its
     * conservative record reservation exactly. Larger configured budgets
     * may complete both generations; smaller budgets must report a blocker.
     */
    for (int i = 0; i < 8063; i++)
        boundary += sprintf("int boundary%d;\n", i);
    for (int i = 0; i < 100; i++)
        traversal += "inherit \"empty\";\n";
    for (int i = 0; i < 100; i++)
        traversal += sprintf("int traversal%d;\n", i);
    cases = ({
        ({"budget boundary", ({boundary, "\n"}), "\n", ({}), 0}),
        ({"inheritance work bound", ({traversal}), traversal, ({}), "SCHEMA_WORK_LIMIT"}),
        ({"own reorder/add/remove", ({"int retained; int removed; int moved;\n"}),
          "int moved; int added; int retained;\n",
          ({({"/schema_target", "$", "retained", 0, 2}), ({"/schema_target", "$", "moved", 2, 0})}), 0}),
        ({"private duplicate", ({"inherit \"base\"; private int duplicate;\n"}),
          "inherit \"base\"; int added; private int duplicate;\n",
          ({({"/base", "$/n0", "duplicate", 0, 0}), ({"/schema_target", "$", "duplicate", 1, 2})}), 0}),
        ({"repeated normal", ({"inherit \"base\"; inherit \"base\"; int own;\n"}),
          "inherit \"base\"; inherit \"base\"; int added; int own;\n",
          ({({"/base", "$/n0", "duplicate", 0, 0}), ({"/base", "$/n1", "duplicate", 1, 1}),
            ({"/schema_target", "$", "own", 2, 3})}), 0}),
        ({"virtual diamond", ({"inherit \"left\"; inherit \"right\"; int own;\n"}),
          "inherit \"left\"; inherit \"right\"; int added; int own;\n",
          ({({"/base", "v:/base", "duplicate", 0, 0}), ({"/left", "$/n0", "duplicate", 1, 1}),
            ({"/right", "$/n1", "duplicate", 2, 2}), ({"/schema_target", "$", "own", 3, 4})}), 0}),
        ({"multiple generations", ({"int retained;\n", "int retained; int removed;\n"}),
          "int added; int retained;\n", ({({"/schema_target", "$", "retained", 0, 1})}), 0}),
        ({"direct reintroduction", ({"int reintroduced;\n", "\n"}),
          "int reintroduced;\n", ({({({"/schema_target", "$", "reintroduced", 0, 0})}), ({})}), 0}),
        ({"unsaved inherited types", ({"inherit \"base\"; int f() { return base_value(\"x\"); }\n"}),
          "inherit \"base\"; int f() { return base_value(\"y\"); }\n",
          ({({"/base", "$/n0", "duplicate", 0, 0})}), 0}),
        ({"parent generation", ({"inherit \"base\";\n"}), "inherit \"base\";\n", ({}), "INHERITANCE_CHANGED"}),
        ({"retype", ({"int value;\n"}), "string value;\n", ({}), "VARIABLE_TYPE_CHANGED"}),
        ({"modifier", ({"int value;\n"}), "private int value;\n", ({}), "VARIABLE_MODIFIERS_CHANGED"}),
        ({"parent graph", ({"inherit \"base\";\n"}), "inherit \"left\";\n", ({}), "INHERITANCE_CHANGED"}),
        ({"weak argument", ({"#pragma weak_types\nf(int x) { return x; }\n"}),
          "#pragma weak_types\nf(string x) { return 1; }\n", ({}), "FUNCTION_SIGNATURE_CHANGED"}),
        ({"function reorder", ({"int f(int x) { return x; } int g() { return 1; }\n"}),
          "int g() { return 2; } int f(int x) { return x + 1; }\n", ({}), 0}),
        ({"inline marker", ({"closure f() { return function int(int x) { return x; }; }\n"}),
          "closure f() { return function int(int x) { return x + 1; }; }\n", ({}), 0}),
        ({"function visibility", ({"int f(int x) { return x; }\n"}),
          "private int f(int x) { return x; }\n", ({}), "FUNCTION_MODIFIERS_CHANGED"}),
        ({"struct layout", ({"struct data { int a; }; struct data value;\n"}),
          "struct data { string a; }; struct data value;\n", ({}), "STRUCT_LAYOUT_CHANGED"})
    });
    next_case();
}
#endif
