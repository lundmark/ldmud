import ldmud

held = []


def hold(first: ldmud.Object, second: ldmud.Object) -> None:
    for obj in (first, second):
        held.extend((obj.functions.discarded, obj.variables.obsolete,
                     iter(obj.functions)))


def drop() -> None:
    held.clear()


ldmud.register_efun("report_python_hold", hold)
ldmud.register_efun("report_python_drop", drop)
