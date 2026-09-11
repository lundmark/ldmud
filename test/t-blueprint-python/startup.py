import gc
import ldmud

handles = ()
views = ()
hashes = ()
keys = {}
keyset = set()
slots = ()
generation_seen = 1
require_shifts = False
blocker = None
blocker_hash = None
finalizations = []
cohort = ()
binding_failures = 0
inventory = []


def current_slots(ob):
    return (ldmud.efuns.to_int(ldmud.LfunClosure(ob, "read_value", ob)),
            ldmud.efuns.to_int(ldmud.IdentifierClosure(ob, "value")))


def python_handles_hold(ob: ldmud.Object, other: ldmud.Object, strict: int) -> int:
    global handles, views, hashes, keys, keyset, slots, generation_seen, cohort, require_shifts
    require_shifts = bool(strict)
    cohort = (ob, other)
    handles = (ob.functions.read_value, ob.variables.value,
               ldmud.LfunClosure(ob, "read_value", ob).lfun,
               ldmud.IdentifierClosure(ob, "value").variable)
    # Each view starts with an independent old cached program. A dir() call
    # on one must not accidentally repair the __dict__ fixture's cache.
    views = (ob.functions, ob.functions, ob.functions,
             ob.variables, ob.variables, ob.variables)
    hashes = tuple(map(hash, handles))
    keys = {handles[0]: "function", handles[1]: "variable"}
    keyset = set(handles)
    slots = current_slots(ob)
    generation_seen = 1
    assert handles[0] == handles[2] and handles[1] == handles[3]
    assert handles[0] != handles[1]
    assert handles[0] != other.functions.read_value
    assert handles[1] != other.variables.value
    assert len(keyset) == 2
    return python_handles_check(ob, 1)


def python_handles_check(ob: ldmud.Object, generation: int) -> int:
    global slots, generation_seen
    assert tuple(map(hash, handles)) == hashes
    fresh = (ob.functions.read_value, ob.variables.value)
    assert handles[0] == handles[2] == fresh[0]
    assert handles[1] == handles[3] == fresh[1]
    assert handles[0] <= fresh[0] and handles[0] >= fresh[0]
    assert not handles[0] < fresh[0] and not handles[0] > fresh[0]
    assert keys[fresh[0]] == keys[handles[2]] == "function"
    assert keys[fresh[1]] == keys[handles[3]] == "variable"
    assert all(item in keyset for item in fresh)
    assert all(item(2) == 75 + generation * 100 for item in (handles[0], handles[2]))
    for fun in (handles[0], handles[2]):
        assert fun.name == "read_value"
        assert fun.return_type == ldmud.Integer
        assert fun.file_name == ob.program_name
        assert fun.line_number > 0
        assert fun.flags == ldmud.LF_NOMASK
        assert fun.visibility == ldmud.VIS_PUBLIC
        assert "read_value" in repr(fun)
        args = fun.arguments
        assert len(args) == 1
        assert args[0].position == 1 and args[0].type == ldmud.Integer
    for var in (handles[1], handles[3]):
        assert var.name == "value" and var.value == 73
        assert var.type == ldmud.Integer and var.flags == ldmud.VF_NOSAVE
        assert var.visibility == ldmud.VIS_PUBLIC
        assert "value" in repr(var)
        var.value = 79
        assert handles[0](1) == 80 + generation * 100
        var.value = 73
    physical = current_slots(ob)
    if generation != generation_seen:
        assert generation == generation_seen + 1
        if require_shifts:
            assert physical[0] != slots[0], ("function did not shift", slots, physical)
            assert physical[1] != slots[1], ("variable did not shift", slots, physical)
        slots, generation_seen = physical, generation
    gc.collect()
    return 1


def python_handles_views(ob: ldmud.Object, generation: int) -> int:
    function_names = ["read_value", "leading"]
    variable_names = ["value", "added"]
    if generation == 3:
        function_names += ["leading_newer", "preceding"]
        variable_names += ["newer", "third"]
    names = dir(views[0])
    assert all(name in names for name in function_names)
    dictionary = views[1].__dict__
    assert all(name in dictionary for name in function_names)
    assert dictionary["read_value"](0) == 73 + generation * 100
    assert dictionary["leading"]() == -123
    assert views[2].read_value(0) == 73 + generation * 100
    names = dir(views[3])
    assert all(name in names for name in variable_names)
    dictionary = views[4].__dict__
    assert all(name in dictionary for name in variable_names)
    assert dictionary["value"].value == 73
    assert dictionary["added"].value == 912
    assert views[5].value.value == 73
    return 1


def python_handles_block(ob: ldmud.Object, kind: int) -> int:
    global blocker, blocker_hash
    if kind == 0:
        blocker = ob.functions.discarded
    elif kind in (1, 2):
        blocker = ob.variables.doomed
    elif kind in (3, 5):
        blocker = iter(ob.functions)
    elif kind in (4, 6):
        blocker = iter(ob.variables)
    else:
        blocker = ob.functions.generated().lfun
    if kind in (5, 6):
        # Do not retain the final item; the exhausted iterator alone blocks.
        for item in blocker:
            pass
    if kind < 3 or kind == 7:
        blocker_hash = hash(blocker)
    return 1


def python_handles_blocker_check(ob: ldmud.Object, kind: int) -> int:
    if kind < 3 or kind == 7:
        assert hash(blocker) == blocker_hash
    if kind == 0:
        assert blocker(1) == 18 and blocker == ob.functions.discarded
    elif kind in (1, 2):
        assert blocker.value == 17 and blocker == ob.variables.doomed
    elif kind in (5, 6):
        assert list(blocker) == []
    elif kind == 7:
        assert blocker() == 19
    return 1


def python_handles_unblock() -> None:
    global blocker
    blocker = None
    gc.collect()


class RetirementObserver:
    def __del__(self):
        # A newly acquired wrapper and both retained targets must already see
        # the new generation when the removed LPC variable drops this value.
        try:
            assert cohort[0].functions.version() == 2
            assert cohort[1].functions.version() == 2
            assert python_handles_check(cohort[0], 2)
            transient = cohort[0].variables.value
            assert transient == handles[1] and transient.value == 73
            finalizations.append("coherent")
        except BaseException as error:
            finalizations.append(repr(error))


def python_handles_retire(ob: ldmud.Object) -> None:
    finalizations.clear()
    ob.functions.store(RetirementObserver())


def python_handles_finalized() -> int:
    assert finalizations == ["coherent"], finalizations
    return 1


def python_handles_destroyed() -> int:
    # Preserve the existing vanished-wrapper semantics; these accesses only
    # prove that stale borrowed bindings are never dereferenced after cleanup.
    for handle in handles:
        assert not handle
        hash(handle)
        assert handle == handle
        try:
            handle.name
        except ValueError:
            pass
        else:
            raise AssertionError("destroyed handle exposes metadata")
    gc.collect()
    return 1


def python_handles_identity_hold(ob: ldmud.Object) -> int:
    global handles, hashes, keys, keyset, slots
    handles = (ob.functions.alpha, ldmud.LfunClosure(ob, "alpha", ob).lfun)
    hashes = tuple(map(hash, handles))
    keys = {handles[0]: "named"}
    keyset = {handles[0]}
    slots = (ldmud.efuns.to_int(ldmud.LfunClosure(ob, "alpha", ob)),)
    assert slots == (1,)
    assert handles[0] == handles[1] and handles[0]() == 41
    return 1


def python_handles_identity_check(ob: ldmud.Object) -> int:
    global handles
    fresh = ob.functions.alpha
    inline = ob.functions.make_inline()
    generated = inline.lfun
    generated_fresh = ob.functions.make_inline().lfun
    assert ldmud.efuns.to_int(inline) == slots[0]
    assert ldmud.efuns.to_int(ldmud.LfunClosure(ob, "alpha", ob)) == 3
    assert tuple(map(hash, handles)) == hashes
    assert hash(fresh) == hashes[0] == hash(generated)
    for named in (*handles, fresh):
        assert named == fresh and not named != fresh
        assert named <= fresh and named >= fresh
        assert not named < fresh and not named > fresh
        assert named() == 141 and keys[named] == "named"
        for raw in (generated, generated_fresh):
            assert named != raw and raw != named
            assert not named == raw and not raw == named
            assert raw < named and raw <= named
            assert not raw > named and not raw >= named
            assert named > raw and named >= raw
            assert not named < raw and not named <= raw
            assert raw() == 19
    assert generated == generated_fresh
    assert generated not in keys and generated not in keyset
    keys[generated] = "generated"
    keyset.add(generated)
    assert len(keys) == len(keyset) == 2
    assert keys[fresh] == "named" and keys[generated_fresh] == "generated"
    assert fresh in keyset and generated_fresh in keyset
    handles = (*handles, fresh, generated, generated_fresh)
    gc.collect()
    return 1


def python_handles_identity_destroyed() -> int:
    assert python_handles_destroyed()
    # Vanished-wrapper semantics remain unchanged; exercise every operation
    # after the target has released both named and generated declarations.
    for left in handles:
        for right in handles:
            _ = (left == right, left != right, left < right,
                 left <= right, left > right, left >= right)
    return 1


def python_handles_probe(ob: ldmud.Object) -> int:
    global binding_failures
    try:
        fun = ob.functions.read
        assert fun() == 19
        var = ob.variables.probe
        assert var.value == 19
    except MemoryError:
        binding_failures += 1
        raise
    return 1


def python_handles_probe_failures() -> int:
    return binding_failures


def python_handles_inventory(ob: ldmud.Object) -> None:
    global inventory
    inventory = [ob.functions for _ in range(6000)]
    assert len({id(view) for view in inventory}) == 6000


def python_handles_drop() -> None:
    global handles, views, keys, keyset, blocker, cohort, inventory
    handles = views = cohort = ()
    keys = {}
    keyset = set()
    blocker = None
    inventory = []
    gc.collect()


for name, function in list(globals().items()):
    if name.startswith("python_handles_"):
        ldmud.register_efun(name, function)
ldmud.register_type("retirement_observer", RetirementObserver)
