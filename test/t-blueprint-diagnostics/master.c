#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/blueprint.inc"

#ifdef __BLUEPRINT_UPDATE__
object blueprint;
int request, phase, checks;

void require(int condition, string message)
{
    checks++;
    if (!condition) raise_error("Blueprint diagnostics: " + message + "\n");
}

void finish(int failed)
{
    if (blueprint) destruct(blueprint);
    rm("target.c");
    shutdown(failed);
}

string candidate_source()
{
    string source = "#pragma strong_types, warn_unused_values\n";

    /* Keep each definition below the lexer line limit. Eight macro notes
     * with long graphemes exceed the old 8 KiB candidate diagnostic buffer.
     * Align definitions so complete source lines survive lexer refills.
     */
    foreach (int index: 8)
    {
        source += "\n" * (2048 - sizeof(to_bytes(source, "UTF-8")) % 2048);
        source += sprintf("#define M%d %s /* e%s */\n", index,
            index ? sprintf("M%d", index - 1) : phase == 1 ? "\"bad\"" : "1",
            "\u0301" * 1000);
    }
    if (phase == 1)
        return source + "int count = M7;\nint value() { return 99; }\n";
    return source + "int value() { " + "M7; " * (phase == 2 ? 5 : 1)
        + "return 99; }\n";
}

void next();
void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        string outcome = blueprint_outcome(report);
        string expected = phase == 0 ? "completed"
                        : phase == 1 ? "COMPILE_FAILED" : "COMPILE_DIAGNOSTIC_LIMIT";

        if (outcome != expected)
            foreach (mapping row: report["errors"])
                msg("%s: %.200s\n", row["code"], row["message"]);
        require(outcome == expected, sprintf("expected %s, got %s", expected, outcome));
        require(blueprint.value() == (phase == 0 ? 99 : 41),
                "warnings allow installation; failures preserve the live program");
        if (phase < 2)
        {
            string code = phase ? "COMPILE_ERROR" : "COMPILE_WARNING";
            int found;
            foreach (mapping row: report["errors"])
                if (row["code"] == code)
                {
                    string message = row["message"];
                    bytes encoded = to_bytes(message, "UTF-8");
                    require(sizeof(encoded) > 8192 && sizeof(encoded) < 16384,
                            "large rendered diagnostic survives candidate collection");
                    require(to_text(encoded, "UTF-8") == message,
                            "rendered diagnostic remains valid UTF-8");
                    found++;
                }
            require(found == 1, "the compiler diagnostic is retained exactly once");
        }
        if (++phase < 3)
            next();
        else
        {
            msg("BLUEPRINT_DIAGNOSTICS: %d checks passed.\n", checks);
            finish(0);
        }
    }); publish);
    if (err) finish(1);
}

void next()
{
    if (blueprint) destruct(blueprint);
    write_file("target.c", "int value() { return 41; }\n", 1, "UTF-8");
    blueprint = load_object("target");
    write_file("target.c", candidate_source(), 1, "UTF-8");
    request = update_blueprint("target", ({}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
#endif

string *epilog(int eflag)
{
#ifdef __BLUEPRINT_UPDATE__
    set_driver_hook(H_FILE_ENCODING, "UTF-8");
    call_out(#'finish, 30 * __ALARM_TIME__, 1);
    next();
#else
    msg("BLUEPRINT_DIAGNOSTICS: blueprint updates disabled; skipped.\n");
    shutdown(0);
#endif
    return 0;
}
