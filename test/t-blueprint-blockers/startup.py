import gc
import ldmud

held = None
finalizations = 0
releasing_binding = False
coherent_finalizations = 0
native_slot = None


class ReferenceChurn:
    def __init__(self, ob):
        self.ob = ob

    def __del__(self):
        global finalizations, coherent_finalizations
        native = ldmud.LfunClosure(self.ob, "read_value", self.ob)
        indexed = self.ob.variables.value
        del native, indexed
        finalizations += 1
        if releasing_binding and held.bound_object == ldmud.get_master():
            coherent_finalizations += 1


def blocker_hold(ob: ldmud.Object, kind: int) -> None:
    global held, native_slot
    if kind == 0:
        held = ob.functions.read_value
    elif kind == 1:
        held = ob.variables.value
    elif kind in (2, 4):
        held = iter(ob.functions)
        if kind == 4:
            for item in held:
                pass
    elif kind in (3, 5):
        held = iter(ob.variables)
        if kind == 5:
            for item in held:
                pass
    elif kind == 6:
        held = ldmud.LfunClosure(ob, "read_value", ldmud.get_master())
    elif kind == 7:
        held = ldmud.IdentifierClosure(ob, "value")
    elif kind == 8:
        held = ldmud.BoundLambdaClosure(ob, ldmud.efuns.unbound_lambda(0, 17))
    elif kind == 9:
        held = ob.functions.handle(4)
    elif kind in (10, 11, 12):
        frame = ob.functions.sleeper(0)
        frame()
        held = frame.variables if kind == 12 else frame
        if kind in (11, 12):
            frame()
    elif kind == 13:
        held = ob.functions
    elif kind == 14:
        held = ob.variables
    elif kind == 15:
        held = ldmud.Closure.__new__(ldmud.Closure, ldmud.get_master(), "read_value", ob)
        assert held.bound_object == ldmud.get_master()
        assert held.object == ob
    else:
        global finalizations, coherent_finalizations
        finalizations = coherent_finalizations = 0
        # The existing GC callback runs after setup temporaries unwind.
        # Unlike ordinary objects, this binding has no object-list root.
        remote = ldmud.LWObject("/python_binding")
        remote.functions.keep(ReferenceChurn(ob))
        held = ldmud.LfunClosure(ob, "read_value", remote)
        del remote
    if kind in (6, 7, 15, 16):
        native_slot = ldmud.efuns.to_int(held)


def blocker_check_native(ob: ldmud.Object, kind: int) -> int:
    assert held.object == ob
    assert held() == 41
    assert ldmud.efuns.to_int(held) != native_slot
    if kind == 7:
        fresh = ldmud.IdentifierClosure(ob, "value")
        assert held == fresh
        indexed = held.variable
        assert indexed.name == "value"
        assert indexed.value == 41
    else:
        assert held.bound_object == ldmud.get_master()
        fresh = ldmud.LfunClosure(ob, "read_value", ldmud.get_master())
        assert held == fresh
        indexed = held.lfun
        assert indexed.name == "read_value"
        assert indexed() == 41
    # Indexed Python handles remain blockers. These conversions
    # are deliberately temporary and unwind before the next publication.
    del indexed, fresh
    return 1


def blocker_rebind() -> None:
    global held, releasing_binding
    releasing_binding = True
    held = ldmud.efuns.bind_lambda(held, ldmud.get_master())
    releasing_binding = False
    assert finalizations == coherent_finalizations == 1, (finalizations, coherent_finalizations)


def blocker_binding_references() -> int:
    binding = held.bound_object
    # Exclude this temporary wrapper and lwobject_info's LPC argument.
    return ldmud.efuns.lwobject_info(binding, -1) - 2


def blocker_drop() -> None:
    global held
    held = None
    gc.collect()


ldmud.register_efun("blocker_hold", blocker_hold)
ldmud.register_efun("blocker_check_native", blocker_check_native)
ldmud.register_efun("blocker_drop", blocker_drop)
ldmud.register_efun("blocker_rebind", blocker_rebind)
ldmud.register_efun("blocker_binding_references", blocker_binding_references)
ldmud.register_efun("blocker_finalizations", lambda: coherent_finalizations)
ldmud.register_type("reference_churn", ReferenceChurn)
