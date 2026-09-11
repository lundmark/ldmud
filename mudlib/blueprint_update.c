/* Reusable state-preserving blueprint update coordinator.
 * Inherit this file and implement all three policy hooks below. The master
 * must independently authorize this coordinator for update_blueprint().
 * See doc/concepts/blueprint_updates for the driver and operator contracts.
 */
#pragma strong_types, init_variables

#ifndef BLUEPRINT_UPDATE_CAPACITY
#define BLUEPRINT_UPDATE_CAPACITY 64
#endif
#ifndef BLUEPRINT_UPDATE_INTERVAL
#define BLUEPRINT_UPDATE_INTERVAL 2
#endif
#ifndef BLUEPRINT_UPDATE_POLL_LIMIT
#define BLUEPRINT_UPDATE_POLL_LIMIT 15
#endif
#ifndef BLUEPRINT_UPDATE_RETENTION
#define BLUEPRINT_UPDATE_RETENTION 600
#endif
#ifndef BLUEPRINT_UPDATE_CLOCK
#define BLUEPRINT_UPDATE_CLOCK time()
#endif
#if BLUEPRINT_UPDATE_CAPACITY < 1 || BLUEPRINT_UPDATE_INTERVAL < 1 \
 || BLUEPRINT_UPDATE_POLL_LIMIT < 1 || BLUEPRINT_UPDATE_RETENTION < 1
#error Blueprint coordinator bounds must be positive
#endif

#define ID 0
#define CALLER 1
#define INITIATOR 2
#define CREATED 3
#define POLLS 4
#define TERMINAL 5
#define BUSY 6
#define FIELDS 7

private mixed **records;
private int scheduled, housekeeping_active;
private mixed *polling_record;

/* Deny by default. Resolve the wizard from the actual immediate caller;
 * accepting a caller-supplied wizard argument would establish no authority.
 */
protected object blueprint_update_initiator(object caller) { return 0; }
protected int blueprint_update_allow_submit(object caller, object initiator,
                                            mixed source, mixed targets) { return 0; }
protected int blueprint_update_allow_result(object caller, object initiator,
                                            int id) { return 0; }

private void clear_record(mixed *record)
{
    for (int i = 0; i < FIELDS; i++) record[i] = 0;
}

private void initialize()
{
    if (records) return;
    mixed **allocated = allocate(BLUEPRINT_UPDATE_CAPACITY);
    for (int i = 0; i < sizeof(allocated); i++) allocated[i] = allocate(FIELDS);
    records = allocated;
}

private void expire_local()
{
    int now = BLUEPRINT_UPDATE_CLOCK;
    foreach (mixed *record : records)
        if (record[ID] > 0 && !record[BUSY]
         && now - record[CREATED] >= BLUEPRINT_UPDATE_RETENTION)
            clear_record(record);
}

private mixed *reserve_record()
{
    mixed *oldest;
    initialize();
    expire_local();
    foreach (mixed *record : records)
    {
        if (!record[ID])
        {
            record[ID] = -1;
            record[BUSY] = 1;
            return record;
        }
        if (record[ID] > 0 && record[TERMINAL] && !record[BUSY]
         && (!oldest || record[CREATED] < oldest[CREATED])) oldest = record;
    }
    if (!oldest) raise_error("Blueprint coordinator: local request capacity exhausted.\n");
    clear_record(oldest);
    oldest[ID] = -1;
    oldest[BUSY] = 1;
    return oldest;
}

private mapping observation(int id, string state, string message)
{
    return (["id": id, "status": state, "message": message]);
}

private void notify(object initiator, string message)
{
    if (initiator) catch(tell_object(initiator, message); nolog);
}

private string message(mapping report)
{
    int id = report["id"];
    switch (report["status"])
    {
    case "pending":
        return sprintf("Blueprint update %d pending.\n", id);
    case "completed":
        return sprintf("Blueprint update %d completed: %d clones updated, %d already current, blueprint %d.\n",
                       id, report["updated"], report["already_current"], report["blueprint_updated"]);
    case "failed":
        return sprintf("Blueprint update %d failed: %s; %d clones matched, none updated.\n",
                       id, report["errors"][0]["code"], report["matched"]);
    default:
        return sprintf("Blueprint update %d %s: %s\n", id, report["status"], report["message"]);
    }
}

private mapping read_record(mixed *record)
{
    mapping report;
    int id = record[ID];
    mixed error = catch(report = update_blueprint_result(id); nolog);
    if (error)
    {
        /* Match the driver's specific unknown-ID error, including catch's
         * system-error prefix. Allocation/evaluation/ownership errors must
         * retain the ID and remain distinguishable from expiry.
         */
        if (error == "*update_blueprint_result(): unknown or expired request id.\n")
        {
            clear_record(record);
            return observation(id, "unknown-expired", "Driver request is unknown or expired.");
        }
        return observation(id, "report-unavailable", "Report read failed; request retained for a later read.");
    }
    record[TERMINAL] = report["status"] != "pending";
    return report;
}

private void housekeeping();

private void schedule()
{
    if (scheduled || housekeeping_active) return;
    foreach (mixed *record : records)
        if (record[ID] > 0)
        {
            call_out(#'housekeeping, BLUEPRINT_UPDATE_INTERVAL);
            scheduled = 1;
            return;
        }
}

private void poll_record(mixed *record)
{
    int id = record[ID];
    object caller = record[CALLER], initiator = record[INITIATOR];
    mapping report;
    if (!caller || !initiator
     || !blueprint_update_allow_result(caller, initiator, id))
    {
        record[POLLS] = BLUEPRINT_UPDATE_POLL_LIMIT;
        return;
    }
    report = read_record(record);
    notify(initiator, message(report));
    if (record[ID] == id && !record[TERMINAL]
     && record[POLLS] >= BLUEPRINT_UPDATE_POLL_LIMIT)
        notify(initiator, sprintf("Blueprint update %d automatic polling stopped; outcome remains unconfirmed. Use result(%d).\n", id, id));
}

private void housekeeping_pass()
{
    expire_local();
    foreach (mixed *record : records)
    {
        if (record[ID] <= 0 || record[BUSY] || record[TERMINAL]
         || record[POLLS] >= BLUEPRINT_UPDATE_POLL_LIMIT) continue;
        polling_record = record;
        record[BUSY] = 1;
        record[POLLS]++;
        catch(poll_record(record); nolog);
        record[BUSY] = 0;
        polling_record = 0;
    }
}

private void housekeeping()
{
    scheduled = 0;
    housekeeping_active = 1;
    /* Keep recovery outside the loop: even an evaluation exception between
     * rows must release our lock and leave manual result access available. */
    catch(housekeeping_pass(); nolog);
    if (polling_record) polling_record[BUSY] = 0;
    polling_record = 0;
    housekeeping_active = 0;
    catch(schedule(); nolog);
}

private void admit(mixed *record, object caller, mixed source, mixed targets)
{
    mixed selection;
    object initiator;
    if (pointerp(targets)) selection = targets + ({});
    else if (targets == 0) selection = 0;
    else raise_error("Blueprint coordinator: selection must be zero or an object array.\n");
    initiator = blueprint_update_initiator(caller);
    if (!caller || !initiator
     || !blueprint_update_allow_submit(caller, initiator, source,
                                      pointerp(selection) ? selection + ({}) : 0))
        raise_error("Blueprint coordinator: submission denied by mudlib policy.\n");
    if (!caller || !initiator)
        raise_error("Blueprint coordinator: caller or initiator was destructed during authorization.\n");
    record[CALLER] = caller;
    record[INITIATOR] = initiator;
    record[CREATED] = BLUEPRINT_UPDATE_CLOCK;
    /* This assignment follows admission with no intervening allocation or
     * callback. The record owns no source or selection references afterward.
     */
    record[ID] = update_blueprint(source, selection);
}

varargs int submit(mixed source, mixed targets)
{
    object caller = previous_object();
    mixed *record = reserve_record();
    mixed error = catch(admit(record, caller, source, targets); nolog);
    if (error || record[ID] <= 0)
    {
        /* throw(0) aborts an LPC catch body while returning zero. Verify the
         * admission postcondition as well as the catch result. */
        clear_record(record);
        raise_error(stringp(error) ? error : "Blueprint coordinator: submission did not produce a request ID.\n");
    }
    int id = record[ID];
    record[BUSY] = 0;
    /* Scheduling and notification are optional after a driver ID exists.
     * Failure here must not hide an admitted request from its operator.
     */
    if (catch(schedule(); nolog))
        catch(notify(record[INITIATOR], sprintf("Blueprint update %d accepted; automatic polling unavailable. Use result(%d).\n", id, id)); nolog);
    else
        catch(notify(record[INITIATOR], sprintf("Blueprint update %d accepted, pending.\n", id)); nolog);
    return id;
}

private mapping authorized_result(mixed *record, object caller)
{
    if (!caller || (caller != record[CALLER] && caller != record[INITIATOR])
     || !record[INITIATOR]
     || !blueprint_update_allow_result(caller, record[INITIATOR], record[ID]))
        raise_error("Blueprint coordinator: result denied by mudlib policy.\n");
    if (!caller || !record[INITIATOR]
     || (caller != record[CALLER] && caller != record[INITIATOR]))
        raise_error("Blueprint coordinator: result owner was destructed during authorization.\n");
    return read_record(record);
}

mapping result(int id)
{
    object caller = previous_object();
    initialize();
    expire_local();
    foreach (mixed *record : records)
        if (record[ID] == id && id > 0)
        {
            mapping report;
            object initiator = record[INITIATOR];
            if (record[BUSY]) raise_error("Blueprint coordinator: request operation is already in progress.\n");
            record[BUSY] = 1;
            mixed error = catch(report = authorized_result(record, caller); nolog);
            record[BUSY] = 0;
            if (error || !mappingp(report))
                raise_error(stringp(error) ? error : "Blueprint coordinator: result authorization did not complete.\n");
            catch(notify(initiator, message(report)); nolog);
            catch(schedule(); nolog);
            return report;
        }
    return observation(id, "unknown-expired", "Local request metadata is unknown or expired.");
}
