inherit "blueprint_update";

object allowed, wizard;
int deny_submit, deny_result, mutate, reenter, nested_rejected;
object extra;
void configure(object caller, object who) { allowed = caller; wizard = who; }
void policy(int submit_denied, int result_denied, int mutation, int recursion, object other)
{
    deny_submit = submit_denied; deny_result = result_denied;
    mutate = mutation; reenter = recursion; extra = other;
}
int rejected() { return nested_rejected; }
protected object blueprint_update_initiator(object caller)
{
    return caller == allowed ? wizard : 0;
}
protected int blueprint_update_allow_submit(object caller, object initiator, mixed source, mixed targets)
{
    if (deny_submit == 2) throw(0);
    if (reenter)
    {
        reenter = 0;
        nested_rejected = !!catch(allowed.submit(this_object(), extra, ({})));
    }
    if (mutate && pointerp(targets) && sizeof(targets)) targets[0] = extra;
    return !deny_submit && caller == allowed && initiator == wizard;
}
protected int blueprint_update_allow_result(object caller, object initiator, int id)
{
    if (deny_result == 2) throw(0);
    return !deny_result && (caller == allowed || caller == wizard) && initiator == wizard;
}
/* Exercise the actual scheduled private closure before the driver executes a
 * freshly admitted request. This test adapter adds no shipped public API. */
int tick()
{
    foreach (mixed *entry : call_out_info())
        if (entry[0] == this_object())
        {
            remove_call_out(entry[1]);
            funcall(entry[1]);
            return 1;
        }
    return 0;
}
