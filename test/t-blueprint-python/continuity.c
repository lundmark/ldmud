#include "common.inc"

#if defined(__BLUEPRINT_UPDATE__) && defined(__PYTHON__)
int generation = 1;

void advance();
void collected(int failed)
{
    if (catch(
        require(!failed, "indexed handle and namespace GC roots"),
        require(python_handles_check(first, generation), "keys and metadata after actual LPC GC"); publish))
    { finish(1); return; }
    if (generation == 3)
    {
        msg("BLUEPRINT_PYTHON_CONTINUITY: %d checks, repeated slot shifts, keys, metadata and views passed.\n", checks);
        finish(0);
    }
    else advance();
}

void inspect()
{
    mapping report = update_blueprint_result(request);
    if (catch(
        require(report["status"] == "completed", sprintf("generation %d expected completed: %O", generation, report["errors"])),
        require(report["updated"] == 2 && report["blueprint_updated"], "entire cohort installed"),
        // Views must inspect before any driver GC refreshes their caches.
        require(python_handles_views(first, generation), "old views refresh independently"),
        require(python_handles_check(first, generation), "stable keys, current calls and metadata"),
        require(second.read_value(0) == 84 + generation * 100, "second object's code and state"),
        swap(first, 3),
        require(python_handles_check(first, generation), "handles resolve after swap"),
        start_gc(#'collected); publish)) finish(1);
}

void advance()
{
    write_generation(++generation, 0);
    request = update_blueprint("python_target", ({first, second}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    done = callback;
    if (catch(setup(), require(python_handles_hold(first, second, 1), "preupdate keys and metadata"),
              advance(); publish)) finish(1);
}
#endif
