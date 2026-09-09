"""Python save-hook regressions, loaded only by Python-enabled test drivers."""
import ldmud


class SaveProbe:
    def __init__(self, action=0):
        self.action = action

    def __save__(self):
        if self.action == 1:
            raise RuntimeError("injected Python save failure")
        if self.action == 2:
            ldmud.efuns.save_value(42)
        if self.action == 3:
            ldmud.efuns.save_object()
        if self.action == 4:
            current = ldmud.efuns.this_object()
            callback = ldmud.Closure(current, "answer", current)
            ldmud.efuns.async_save_object("nested-python", callback)
        if self.action == 5:
            ldmud.efuns.destruct(ldmud.efuns.this_object())
        return ldmud.Array((42, "Python payload"))

    @staticmethod
    def __restore__(value):
        if list(value) != [42, "Python payload"]:
            raise ValueError("incorrect restored Python payload")
        return SaveProbe()


def make_save_probe(action: int) -> SaveProbe:
    return SaveProbe(action)


ldmud.register_type("save_probe", SaveProbe)
ldmud.register_efun("make_save_probe", make_save_probe)
