import contextvars
import ldmud

context_step = -1
context_finalized = 0


class ContextLifetime:
    def __del__(self):
        global context_finalized
        context_finalized += 1


def context_start() -> None:
    global context_step
    context_step = 0


def context_replace() -> int:
    # Entering this Python efun replaced the sole LW ContextVar with master.
    assert context_finalized == 0, "restored LW context was freed inside its secure call"
    return 1


def context_heartbeat():
    global context_step, context_finalized
    master = ldmud.get_master()
    master.functions.batch_boundary()
    if context_step < 0:
        return
    var = next(var for var in contextvars.copy_context() if var.name == "ldmud.current_object")
    context_finalized = 0
    remote = ldmud.LWObject("/remote")
    remote.functions.keep(ContextLifetime())
    master.functions.drain_trace()
    var.set(remote)
    del remote
    try:
        master.functions.context_nested(context_step)
        assert context_step == 0
    except RuntimeError as error:
        assert context_step == 1 and "expected LW context error" in str(error)
    assert context_finalized == 0, "context wrapper still owns the restored LW"
    var.set(None)
    assert context_finalized == 1, "restored context leaked a counted LW reference"
    context_step += 1
    if context_step == 2:
        context_step = -1
        master.functions.context_done()


class Retired:
    def __init__(self, kind):
        self.kind = kind

    def __del__(self):
        master = ldmud.get_master()
        master.functions.observe(self.kind)
        if self.kind == 7:
            # A Python exception is unraisable; it must not escape Py_DECREF.
            raise ValueError("expected retirement Python exception")
        if self.kind == 8:
            # The LPC failure passes through call_lpc_secure and runtime_error.
            master.functions.callback_error()


def retirement_seed(ob: ldmud.Object, kind: int) -> None:
    ob.functions.hold(Retired(kind), kind)


ldmud.register_type("retired", Retired)
ldmud.register_efun("retirement_seed", retirement_seed)
ldmud.register_type("context_lifetime", ContextLifetime)
ldmud.register_efun("context_start", context_start)
ldmud.register_efun("context_replace", context_replace)
ldmud.register_hook(ldmud.ON_HEARTBEAT, context_heartbeat)
