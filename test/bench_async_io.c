/* Matched synchronous and asynchronous file/save measurements.
 * Run through runbench_async_io.sh, which supplies bench_config.h and an
 * isolated writable mudlib. Only payload is part of the saved snapshot.
 */

#define OWN_RUNTIME_WARNING
#include "/inc/base.inc"
#include "/sys/driver_info.h"
#include "/sys/object_info.h"
#include "/bench_config.h"

mixed payload;
nosave object verifier;
nosave string *kinds = ({ "scalars", "strings", "nested", "shared_cycles" });
nosave string *modes = BENCH_MODES;
nosave string prepared_text, current_kind, current_mode;
nosave int output_bytes, fixture_index, fixture_ready, repetition, mode_index;
nosave int *sample_started;
nosave int sample_submit_us, sample_user_ms, sample_system_ms;
nosave int sample_before_memory, sample_after_memory, sample_returned;
nosave int probe_number, probe_batch, probe_completed, probe_observed;
nosave int probe_returned, probe_inline, probe_at_completed, probe_after_memory;
nosave int *probe_started, *probe_submits, *probe_completions;
nosave int probe_block_us, probe_delay_us, probe_total_us;
nosave mixed *probe_starts, *probe_results;
nosave string *probe_paths;

/* Subtract seconds before converting to microseconds, including on 32 bit.
 * The harness bounds each run with a timeout; no epoch-sized integer/float
 * is needed. utime() is a wall clock, so reject backward adjustments.
 */
int elapsed_us(int *start, int *end)
{
    int elapsed = (end[0] - start[0]) * 1000000 + end[1] - start[1];
    if (elapsed < 0)
        raise_error("Wall clock moved backwards during benchmark.\n");
    return elapsed;
}

void require(int condition, string message)
{
    if (!condition)
        raise_error(message + "\n");
}

/* Array/mapping identity is checked in both directions. Remember aggregates
 * before descending, so aliases and cycles terminate and must be preserved.
 * Fixture mapping keys are scalars and mappings have one value per key.
 */
void same_graph(mixed expected, mixed actual, mapping forward, mapping reverse)
{
    if (pointerp(expected) || mappingp(expected))
    {
        require(pointerp(expected) == pointerp(actual)
             && mappingp(expected) == mappingp(actual), "Aggregate type changed");
        if (member(forward, expected))
        {
            require(forward[expected] == actual, "Alias or cycle was lost");
            return;
        }
        require(!member(reverse, actual), "Distinct aggregates were merged");
        forward[expected] = actual;
        reverse[actual] = expected;
        require(sizeof(expected) == sizeof(actual), "Aggregate size changed");
        if (pointerp(expected))
        {
            for (int i = 0; i < sizeof(expected); i++)
                same_graph(expected[i], actual[i], forward, reverse);
        }
        else
        {
            foreach (mixed key, mixed value : expected)
            {
                require(member(actual, key), "Mapping key was lost");
                same_graph(value, actual[key], forward, reverse);
            }
        }
        return;
    }
    require(get_type_info(expected, 0) == get_type_info(actual, 0)
         && expected == actual, "Scalar value or type changed");
}

mixed restored_payload(string source)
{
    payload = 0;
    require(restore_object(source) == 1, "restore_object failed");
    return payload;
}

/* Refcounts alone cannot reclaim cycles. Break fixture links after checking
 * them, outside the measured interval, instead of accumulating garbage from
 * restores until the backend next runs its collector.
 */
void clear_graph(mixed node, mapping seen)
{
    if ((!pointerp(node) && !mappingp(node)) || member(seen, node))
        return;
    seen[node] = 1;
    if (pointerp(node))
    {
        for (int i = 0; i < sizeof(node); i++)
        {
            clear_graph(node[i], seen);
            node[i] = 0;
        }
    }
    else
    {
        foreach (mixed key, mixed value : node)
        {
            clear_graph(value, seen);
            node[key] = 0;
        }
    }
}

void release_payload()
{
    clear_graph(payload, ([]));
    payload = 0;
}

void make_fixture(string kind)
{
    release_payload();
    switch (kind)
    {
    case "scalars":
        payload = allocate(BENCH_SCALARS);
        for (int i = 0; i < BENCH_SCALARS; i++)
            switch (i % 3)
            {
            case 0: payload[i] = i * 17 - 100000; break;
            case 1: payload[i] = to_float(i) / 7.0; break;
            case 2: payload[i] = sprintf("scalar-%d", i); break;
            }
        break;

    case "strings":
        string text = "abcdefghijklmnopqrstuvwxyz0123456789\"\\\n";
        while (sizeof(text) < BENCH_STRING_BYTES)
            text += text;
        text = text[0..BENCH_STRING_BYTES - 1];
        payload = allocate(BENCH_STRINGS);
        for (int i = 0; i < BENCH_STRINGS; i++)
            payload[i] = sprintf("%d:", i) + text;
        break;

    case "nested":
        payload = allocate(BENCH_NESTED_WIDTH);
        for (int i = 0; i < BENCH_NESTED_WIDTH; i++)
        {
            mixed node = ({ i, "leaf", i * 3 });
            for (int depth = 0; depth < BENCH_NESTED_DEPTH; depth++)
                node = ([ "depth": depth, "index": i,
                          "children": ({ node, ({ i + depth, "side" }) }) ]);
            payload[i] = node;
        }
        break;

    case "shared_cycles":
        mixed *shared = ({ "shared", 12345, ({ "inner", 42 }) });
        mapping cycle = ([ "shared": shared ]);
        mixed *ring = ({ cycle, 0 });
        ring[1] = ring;
        cycle["self"] = cycle;
        cycle["ring"] = ring;
        payload = allocate(BENCH_SHARED);
        for (int i = 0; i < BENCH_SHARED; i++)
            payload[i] = ({ shared, shared, cycle, ring, ({ i, i }) });
        break;

    default:
        raise_error("Unknown fixture\n");
    }
}

/* Payload-capacity accounting rounds allocations upward. Reserve at most
 * twice the output size (with a 4096-byte floor), capped by the configured
 * per-request limit, to keep bursts within the supplied queue bounds.
 */
int request_capacity()
{
    return min(BENCH_QUEUE_REQUEST_BYTES,
               max(4096, output_bytes > BENCH_QUEUE_REQUEST_BYTES / 2
                         ? BENCH_QUEUE_REQUEST_BYTES : output_bytes * 2));
}

int asynchronous(string mode)
{
    return mode == "async_write" || mode == "async_save";
}

void prepare_fixture(string kind, int record)
{
    string text;
    prepared_text = 0;
    make_fixture(kind);
    if (record)
        debug_message(sprintf("FIXTURE %s %d %d\n", kind,
                              object_info(this_object(), OI_DATA_SIZE),
                              object_info(this_object(), OI_DATA_SIZE_TOTAL)));
    text = save_object(3);
    output_bytes = sizeof(to_bytes(text, "UTF-8"));
    if (member(modes, "write_file") >= 0 || member(modes, "async_write") >= 0)
        prepared_text = text;
    if (member(modes, "async_write") >= 0 || member(modes, "async_save") >= 0)
        require(output_bytes <= BENCH_QUEUE_REQUEST_BYTES,
                "Fixture exceeds the supplied per-request queue limit");
}

/* The only timed work is the call and this common dispatch wrapper. Neither
 * prepared-text generation nor validation runs inside a call's interval.
 */
mixed save_once(string mode, string path, closure callback)
{
    switch (mode)
    {
    case "string": return save_object(3);
    case "file": return save_object(path, 3);
    case "write_file": return write_file(path + ".o", prepared_text, 1);
#if BENCH_HAS_ASYNC
    case "async_write":
        async_write(path + ".o", prepared_text, 1, callback);
        return 0;
    case "async_save":
        async_save_object(path, callback, 3);
        return 0;
#endif
    }
    raise_error("Unknown benchmark mode\n");
    return 0;
}

void validate_output(string mode, mixed result, string path)
{
    string source;
    int size;
    if (mode == "string")
    {
        require(stringp(result), "String save_object failed");
        source = result;
        size = sizeof(to_bytes(source, "UTF-8"));
    }
    else
    {
        require(result == (mode == "write_file" ? 1 : 0),
                "File operation or asynchronous completion failed: " + mode);
        size = file_size(path + ".o");
        source = path;
        if (mode == "write_file" || mode == "async_write")
            require(read_bytes(path + ".o") == to_bytes(prepared_text, "UTF-8"),
                    "Prepared text changed in the written output");
    }
    require(size == output_bytes, "Matched output size changed");
    same_graph(payload, verifier->restored_payload(source), ([]), ([]));
    verifier->release_payload();
}

void record_sample(int completion_us)
{
    if (repetition >= 0)
        debug_message(sprintf("SAMPLE %s %s %d %d %d %d %d %d %d %d\n",
            current_kind, current_mode, repetition, sample_submit_us,
            completion_us, output_bytes, sample_user_ms, sample_system_ms,
            sample_before_memory, sample_after_memory));
}

void next_sample()
{
    if (++mode_index == sizeof(modes))
    {
        mode_index = 0;
        if (++repetition == BENCH_REPETITIONS)
        {
            fixture_index++;
            fixture_ready = 0;
        }
    }
}

void run_samples();
void probe_start();

void sample_done(int result)
{
    int completion_us = elapsed_us(sample_started, utime());
    require(sample_returned, "Asynchronous callback ran during submission");
    validate_output(current_mode, result, "/snapshot");
    record_sample(completion_us);
    next_sample();
    /* The finishing request still occupies one queue slot until this
     * callback returns. Small custom queues need a separate backend turn.
     */
    if (BENCH_QUEUE_REQUESTS < 2
     || BENCH_QUEUE_REQUEST_BYTES > BENCH_QUEUE_BYTES / 2)
        call_out(#'run_samples, 0);
    else
        run_samples();
}

void run_samples()
{
    while (fixture_index < sizeof(kinds))
    {
        int *before_cpu, *after_cpu;
        mixed result;
        closure callback = #'sample_done;
        if (!fixture_ready)
        {
            current_kind = kinds[fixture_index];
            prepare_fixture(current_kind, 1);
            repetition = -BENCH_WARMUP;
            mode_index = 0;
            fixture_ready = 1;
        }
        current_mode = modes[(mode_index + repetition + BENCH_WARMUP
                              + BENCH_RUN_NUMBER) % sizeof(modes)];
        before_cpu = rusage();
        sample_before_memory = driver_info(DI_SIZE_MEMORY_USED);
        sample_returned = 0;
        sample_started = utime();
        result = save_once(current_mode, "/snapshot", callback);
        sample_submit_us = elapsed_us(sample_started, utime());
        sample_after_memory = driver_info(DI_SIZE_MEMORY_USED);
        after_cpu = rusage();
        sample_user_ms = after_cpu[0] - before_cpu[0];
        sample_system_ms = after_cpu[1] - before_cpu[1];
        sample_returned = 1;
        if (asynchronous(current_mode))
            return;
        validate_output(current_mode, result, "/snapshot");
        record_sample(-1);
        next_sample();
    }
    /* Release the final completed request before admitting the burst. */
    call_out(#'probe_start, 0);
}

void benchmark_done()
{
    int *cpu = rusage();
    debug_message(sprintf("PROCESS_CPU %d %d\n", cpu[0], cpu[1]));
    debug_message("BENCH_DONE\n");
    /* Leave the drained helper alive briefly for /proc resource sampling.
     * This settling interval is outside every measured latency window.
     */
    call_out(#'shutdown, 1, 0);
}

void finish_probe()
{
    require(!probe_inline, "Burst callback ran during submission");
    for (int i = 0; i < probe_batch; i++)
    {
        validate_output(current_mode, probe_results[i], probe_paths[i]);
        debug_message(sprintf("BURST_SAMPLE strings %s %d %d %d %d\n",
                              current_mode, i, probe_submits[i],
                              probe_completions[i], output_bytes));
    }
    debug_message(sprintf("LOOP strings %s %d %d %d %d %d %d\n",
                          current_mode, probe_batch, probe_block_us,
                          probe_delay_us, probe_total_us, probe_at_completed,
                          probe_after_memory));
    probe_results = 0;
    if (++probe_number < sizeof(modes))
        call_out(#'probe_start, 0);
    else
        benchmark_done();
}

void probe_tick()
{
    probe_delay_us = elapsed_us(probe_started, utime());
    probe_at_completed = probe_completed;
    probe_observed = 1;
    if (probe_completed == probe_batch)
        finish_probe();
}

void burst_done(int result)
{
    int *now = utime();
    int index = probe_completed++;
    /* Store timestamps/status only until all completion windows have ended.
     * Output checks, printing and fixture cleanup happen in finish_probe().
     */
    probe_completions[index] = elapsed_us(probe_starts[index], now);
    probe_results[index] = result;
    if (!probe_returned)
        probe_inline = 1;
    if (probe_completed == probe_batch)
    {
        probe_total_us = elapsed_us(probe_started, now);
        if (probe_observed)
            finish_probe();
    }
}

/* The callout measures when the backend gets another chance to run after
 * submissions. It can include one-second timer granularity. Burst completion
 * is measured separately, and completed_at_probe shows whether work remained.
 */
void probe_start()
{
    closure callback = #'burst_done;
    if (!probe_number)
    {
        prepare_fixture("strings", 0);
        probe_batch = BENCH_LOOP_BATCH;
        if (member(modes, "async_write") >= 0 || member(modes, "async_save") >= 0)
            probe_batch = min(probe_batch,
                              min(BENCH_QUEUE_REQUESTS,
                                  BENCH_QUEUE_BYTES / request_capacity()));
        require(probe_batch > 0, "Queue limit cannot hold one burst request");
    }
    current_mode = modes[(probe_number + BENCH_RUN_NUMBER) % sizeof(modes)];
    probe_paths = allocate(probe_batch);
    probe_starts = allocate(probe_batch);
    probe_results = allocate(probe_batch);
    probe_submits = allocate(probe_batch);
    probe_completions = allocate(probe_batch);
    for (int i = 0; i < probe_batch; i++)
        probe_paths[i] = sprintf("/burst-%s-%d", current_mode, i);
    probe_completed = probe_observed = probe_returned = probe_inline = 0;
    probe_started = utime();
    call_out(#'probe_tick, 0);
    for (int i = 0; i < probe_batch; i++)
    {
        probe_starts[i] = utime();
        probe_results[i] = save_once(current_mode, probe_paths[i], callback);
        probe_submits[i] = elapsed_us(probe_starts[i], utime());
        probe_completions[i] = -1;
    }
    probe_block_us = elapsed_us(probe_started, utime());
    probe_after_memory = driver_info(DI_SIZE_MEMORY_USED);
    probe_returned = 1;
    if (!asynchronous(current_mode))
    {
        probe_completed = probe_batch;
        probe_total_us = probe_block_us;
    }
}

void runtime_error(string message, string program, string current, int line)
{
    debug_message(sprintf("BENCH_ERROR %s:%d: %s\n", program, line, message));
    shutdown(1);
}

void runtime_warning(string message, string current, string program,
                     int line, int caught)
{
    debug_message(sprintf("BENCH_WARNING %s:%d: %s\n", program, line, message));
    shutdown(1);
}

string *epilog(int eflag)
{
    verifier = clone_object("/bench_async_io");
    run_samples();
    return 0;
}
