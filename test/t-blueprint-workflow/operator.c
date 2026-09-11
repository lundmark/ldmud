object wizard;
void configure(object ob) { wizard = ob; }
object initiator() { return wizard; }
varargs int submit(object coordinator, mixed source, mixed targets)
{
    return coordinator.submit(source, targets);
}
mapping result(object coordinator, int id) { return coordinator.result(id); }

mapping direct_result(int id) { return update_blueprint_result(id); }
