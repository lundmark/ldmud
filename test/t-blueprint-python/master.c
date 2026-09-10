#include "/inc/base.inc"

string *cases = ({"continuity", "blockers", "lifecycle", "faults", "limits", "identity"});
int current;

mixed include_file(string path, string from, int system)
{
    if (path == "python_binding_hook.h")
        return load_object("faults").include_file(path, from, system);
    return 0;
}

void next(int failed)
{
    if (failed) { shutdown(1); return; }
#ifdef TASK32_CASE
    if (current) { shutdown(0); return; }
    current++;
    load_object(cases[TASK32_CASE]).run(#'next);
#else
    if (current == sizeof(cases)) { shutdown(0); return; }
    load_object(cases[current++]).run(#'next);
#endif
}

string *epilog(int eflag)
{
#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
    call_out(#'shutdown, 1200, 1);
    if (catch(next(0); publish)) shutdown(1);
#else
    msg("BLUEPRINT_PYTHON_HANDLES: feature or Python disabled.\n");
    shutdown(0);
#endif
    return 0;
}
