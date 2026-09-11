#include "/inc/base.inc"

int observed_release;
int released_before_error() { return observed_release; }
void runtime_error(string error, string program, string current, int line, mixed culprit, int caught)
{
    if (strstr(error, "injected partial variable") >= 0)
        observed_release = read_file("migration-released") == "1\n";
}

void run_test()
{
#ifdef __BLUEPRINT_UPDATE__
#ifdef MIGRATION_ONLY_FAULTS
    load_object("faults").run(#'shutdown);
#else
    load_object("migration").run(function void(int failed)
    {
        if (failed) shutdown(1);
        else load_object("aliases").run(function void(int failed)
        {
            if (failed) shutdown(1);
            else load_object("generations").run(function void(int failed)
            {
                if (failed) shutdown(1);
                else load_object("faults").run(#'shutdown);
            });
        });
    });
#endif
#else
    msg("BLUEPRINT_MIGRATION: feature disabled.\n");
    shutdown(0);
#endif
}

string *epilog(int eflag) { run_test(); return 0; }
