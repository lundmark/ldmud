/* Mutable strings keep weak lists of their character and range lvalues.
 * Reclaiming an unreachable alias must leave only surviving list entries.
 */
#include "/inc/msg.inc"
#include "/sys/driver_info.h"

#define GC_LOG "/cycles.gc.log"

closure finished;
mixed *values, *chars, *ranges, *char_aliases, *range_aliases;
int phase, baseline_lvalues, live_lvalues;

void require(int condition, string description)
{
    if (!condition) raise_error(description + "\n");
}

void poll_gc(int retried);

void collect(int next_phase)
{
    phase = next_phase;
    rm(GC_LOG);
    garbage_collection(GC_LOG);
    call_out(#'poll_gc, __ALARM_TIME__, 0);
}

void make_garbage(int range)
{
    mixed *garbage = ({ 0, 0, 0 });
    garbage[0] = garbage;
    for (int i = 0; i < 2; i++)
        if (range) garbage[i+1] = &(values[i][<1..<1]);
        else garbage[i+1] = &(values[i][<1]);
    /* Leaving this frame retains only the array's self-reference and its
     * two distinct aliases. None can be freed by reference counting.
     */
}

void advance(string log)
{
    int freed = sizeof(regexp(explode(log, "\n"), "freeing.*block"));
    require(strstr(log, "--- Garbage Collection ---") >= 0,
            "Cycle test requires an actual garbage collection");

    if (!phase)
    {
        require(!freed, "Unexpected lost block before cycle test");
        baseline_lvalues = driver_info(DI_NUM_LVALUES);
        values = ({ "abcd", b"abcd" });
        chars = allocate(2);
        ranges = allocate(2);
        char_aliases = allocate(2);
        range_aliases = allocate(2);
        for (int i = 0; i < 2; i++)
        {
            chars[i] = &(values[i][1]);
            ranges[i] = &(values[i][2..2]);
            char_aliases[i] = &(chars[i]);
            range_aliases[i] = &(ranges[i]);
        }
        /* Normalize temporary indexing protectors before taking the live
         * count used to distinguish retained roots from reclaimed garbage.
         */
        collect(1);
        return;
    }

    if (phase == 1)
    {
        require(!freed, "Unexpected lost block while establishing live aliases");
        live_lvalues = driver_info(DI_NUM_LVALUES);
        make_garbage(0);
        collect(2);
        return;
    }

    if (phase <= 3)
    {
        /* Exactly the unreachable array and its two lvalues are garbage.
         * The count also rejects accidentally keeping weak links as roots.
         */
        require(freed == 3, "Expected only the array and two unreachable aliases");
        require(driver_info(DI_NUM_LVALUES) == live_lvalues,
                "Garbage aliases reclaimed and all live lvalues retained");

        chars[0] = phase == 2 ? 0x1f600 : 'Q';
        chars[1] = phase == 2 ? 'Z' : 'Q';
        require(char_aliases[0] == chars[0] && char_aliases[1] == chars[1],
                "Character aliases survive weak-list collection");
        require(ranges[0] == (phase == 2 ? "c" : "XY")
                && ranges[1] == (phase == 2 ? b"c" : b"XY"),
                "Live range positions follow character width changes");

        ranges[0] = phase == 2 ? "XY" : "Z";
        ranges[1] = phase == 2 ? b"XY" : b"Z";
        require(range_aliases[0] == ranges[0] && range_aliases[1] == ranges[1],
                "Range aliases survive weak-list collection");
        require(values[0] == (phase == 2 ? "a\U0001f600XYd" : "aQZd")
                && values[1] == (phase == 2 ? b"aZXYd" : b"aQZd"),
                "Resized aliases still update their backing variables");

        msg("GC_LVALUE_CYCLES: phase %d reclaimed exactly three blocks.\n", phase);
        if (phase == 2)
        {
            make_garbage(1);
            collect(3);
            return;
        }
        values = chars = ranges = char_aliases = range_aliases = 0;
        collect(4);
        return;
    }

    require(!freed, "Unexpected lost block after releasing cycle survivors");
    require(driver_info(DI_NUM_LVALUES) == baseline_lvalues,
            "All surviving lvalues released");
    msg("GC_LVALUE_CYCLES: all stages passed.\n");
    funcall(finished, 0);
}

void poll_gc(int retried)
{
    string log = read_file(GC_LOG);
    if (!log && !retried)
    {
        call_out(#'poll_gc, __ALARM_TIME__, 1);
        return;
    }
    rm(GC_LOG);
    if (!log && !phase)
    {
        /* Probe before constructing cycles: sysmalloc cannot reclaim them. */
        msg("GC_LVALUE_CYCLES: skipped (allocator has no tracing GC).\n");
        funcall(finished, 0);
        return;
    }
    if (!log || catch(advance(log); publish))
        funcall(finished, 1);
}

void run(closure callback)
{
    finished = callback;
    collect(0);
}
