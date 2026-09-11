"""Optional Python handle cohort for the explicit blueprint stress mudlib."""
import gc
import sys
import tracemalloc
import ldmud

cohort = ()
handles = ()
hashes = ()


def stress_python_hold(objects: ldmud.Array) -> int:
    global cohort, handles, hashes
    gc.collect()
    tracemalloc.start()
    start, _ = tracemalloc.get_traced_memory()
    cohort = tuple(objects)
    handles = tuple((ob.functions.read_value, ob.variables.v0) for ob in cohort)
    hashes = tuple((hash(fun), hash(var)) for fun, var in handles)
    current, peak = tracemalloc.get_traced_memory()
    shallow = sum(sys.getsizeof(handle) for pair in handles for handle in pair)
    print(f"BLUEPRINT_STRESS_PYTHON_MEMORY: count={len(cohort)} wrappers_shallow={shallow} traced_current_delta={current-start} traced_peak_delta={peak-start}", flush=True)
    tracemalloc.stop()
    return 1


def stress_python_check(generation: int) -> int:
    for index, (ob, pair, stored_hashes) in enumerate(zip(cohort, handles, hashes)):
        fun, var = pair
        assert ob.functions.version() == generation
        assert fun() == index and var.value == index
        assert fun == ob.functions.read_value and var == ob.variables.v0
        assert (hash(fun), hash(var)) == stored_hashes
    return 1


ldmud.register_efun("stress_python_hold", stress_python_hold)
ldmud.register_efun("stress_python_check", stress_python_check)
