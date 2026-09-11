/* Protected aliases must retain their storage and write-through semantics
 * across repeated collections, owner destruction, and alias release.
 */
#include "/inc/base.inc"
#include "/inc/gc.inc"

object *cases = ({});
int stage;
int no_gc;
int with_cycles;

void advance(int failed);

void collect()
{
    if (no_gc) call_out(#'advance, 0, 0);
    else start_gc(#'advance);
}

void advance(int failed)
{
    if (failed)
    {
        shutdown(1);
        return;
    }
    stage++;
    if (stage == 4)
    {
        foreach (object fixture: cases)
            if (catch(fixture->release(); publish))
            {
                shutdown(1);
                return;
            }
        cases = ({});
    }
    else if (stage == 5)
    {
        msg("GC_LVALUES: all stages passed%s.\n", no_gc ? " (without GC)" : "");
        if (!with_cycles) shutdown(0);
        else if (catch(load_object("cycles")->run(#'shutdown); publish))
            shutdown(1);
        return;
    }
    else
    {
        foreach (object fixture: cases)
            if (catch(fixture->check(stage); publish))
            {
                shutdown(1);
                return;
            }
        msg("GC_LVALUES: stage %d passed for %d cases.\n", stage, sizeof(cases));
    }
    collect();
}

void flag(string selection)
{
    rm(__MASTER_OBJECT__ ".gc.log");
    if (selection == "cycles")
    {
        if (catch(load_object("cycles")->run(#'shutdown); publish))
            shutdown(1);
        return;
    }
    no_gc = selection == "no-gc";
    with_cycles = selection == "test";
    foreach (string kind: ({ "string-range", "bytes-range", "array-range",
                            "string-char", "bytes-char", "unicode-char" }))
        foreach (string mode: ({ "attached", "detached", "destroyed", "temporary", "retyped" }))
        {
            if (selection != "test" && selection != "lvalues"
                && !no_gc && selection != kind + "/" + mode)
                continue;
            object fixture;
            if (catch(fixture = clone_object("case"),
                      fixture->setup(kind, mode); publish))
            {
                shutdown(1);
                return;
            }
            cases += ({ fixture });
        }
    if (!sizeof(cases))
    {
        shutdown(1);
        return;
    }
    msg("GC_LVALUES: initialized %d cases before collection.\n", sizeof(cases));
    collect();
}
