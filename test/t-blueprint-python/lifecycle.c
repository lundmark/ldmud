#include "common.inc"

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
void destroyed(int failed)
{
    if (catch(
        require(!failed, "destroyed targets and weak wrappers collect cleanly"),
        require(python_handles_destroyed(), "destroyed handles never read freed bindings"); publish))
    { finish(1); return; }
    msg("BLUEPRINT_PYTHON_LIFETIME: %d checks, retirement finalizer and destroyed-wrapper GC passed.\n", checks);
    finish(0);
}

void inspect()
{
    mapping report = update_blueprint_result(request);
    if (catch(
        require(report["status"] == "completed", sprintf("retirement migration: %O", report["errors"])),
        require(python_handles_finalized(), "finalizer observes coherent entire cohort and handles"),
        destruct(first), destruct(second), destruct(source),
        start_gc(#'destroyed); publish)) finish(1);
}

void run(closure callback)
{
    done = callback;
    if (catch(
        setup(), require(python_handles_hold(first, second, 0), "retained keys before finalizer"),
        python_handles_retire(first), write_generation(2, 4),
        request = update_blueprint("python_target", ({first, second})),
        call_out(#'inspect, __ALARM_TIME__ + 1); publish)) finish(1);
}
#endif
