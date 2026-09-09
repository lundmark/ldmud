#pragma strong_types, save_types
#include "/inc/base.inc"
#include "/inc/gc.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object *families = ({}), *owners = ({});
int *ids = ({});
int round, checks, reenter, overlap, global_reentry;
int pending_id;

void require(int condition, string message)
{
    checks++;
    if (!condition) raise_error("Request cache: " + message + "\n");
}

void cleanup()
{
    foreach (object owner: owners) if (owner) destruct(owner);
    foreach (object family: families) if (family) destruct(family);
    if (find_object("request_owner")) destruct(find_object("request_owner"));
    for (int i = 0; i < 65; i++) rm("requests_family" + i + ".c");
    rm("request_owner.c"); rm("requests-clock"); rm("requests-last-id");
}

void authorize(mixed source)
{
    if (!reenter) return;
    reenter = 0;
    overlap = !!catch(update_blueprint(source, ({})));
    global_reentry = !!catch(update_blueprint(families[64], ({})));
}

void submit_round();
void collected(int failed)
{
    mixed err = catch(funcall(function void()
    {
        require(!failed, "cache cleanup passes GC and reference accounting");
        require(!!catch(update_blueprint_result(pending_id)), "destroyed owner removes pending identity");
        msg("BLUEPRINT_REQUEST_CACHE: %d checks passed.\n", checks);
    }); publish);
    cleanup();
    funcall(done, !!err);
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        int last = ids[<1];
        require(owners[7].result(last)["completed_at"] == 2000000000,
                "private clock fixes completion time without changing backend time");
        require(owners[7].result(last)["status"] == "failed", "all detached requests terminalized");
        if (++round < 5)
        {
            submit_round();
            return;
        }
        require(sizeof(ids) == 320, "cache exceeded the actual 256-report capacity");
        for (int i = 0; i < 64; i++)
            require(!!catch(owners[i / 8].result(ids[i])), "oldest equal-time lower ID evicted");
        require(owners[0].result(ids[64])["id"] == ids[64], "oldest retained cache boundary");
        require(!!catch(owners[1].result(last)), "cache owner isolation");
        pending_id = owners[0].submit(families[0]);
        require(pending_id == last + 1, "rejected capacity admissions never consume IDs");
        require(owners[0].result(pending_id)["status"] == "pending", "terminal cache does not consume pending capacity");
        rm("requests-clock"); write_file("requests-clock", "2000000299\n");
        require(owners[7].result(last)["completed_at"] == 2000000000, "poll at TTL minus one does not renew");
        require(owners[7].result(last)["completed_at"] == 2000000000, "repeated poll does not renew");
        rm("requests-clock"); write_file("requests-clock", "2000000300\n");
        require(!!catch(owners[7].result(last)), "expired exactly at the 300 second TTL");
        require(owners[0].result(pending_id)["status"] == "pending", "expiry leaves pending requests owned");
        write_file("requests-last-id", "max\n");
        require(!!catch(owners[1].submit(families[1])), "request ID exhaustion rejects before admission");
        require(owners[0].result(pending_id)["status"] == "pending", "ID exhaustion preserves prior ownership");
        foreach (object owner: owners) destruct(owner);
        start_gc(#'collected);
    }); publish);
    if (err) { cleanup(); funcall(done, 1); }
}

void submit_round()
{
    int previous = sizeof(ids) ? ids[<1] : 0;
    for (int i = 0; i < 64; i++)
    {
        if (i == 63) reenter = 1;
        ids += ({owners[i / 8].submit(families[i])});
        if (i == 7)
            require(!!catch(owners[0].submit(families[64])), "exact per-owner eight-request limit");
    }
    require(overlap && global_reentry, "unpublished and detached requests reserve family/global capacity during reentry");
    require(!!catch(update_blueprint(families[64], ({}))), "exact global 64-request limit");
    require(!!catch(owners[7].submit(families[0])), "family reservation covers other owners and disjoint selections");
    if (previous) require(ids[<64] == previous + 1, "terminalization releases capacity without consuming rejected IDs");
    call_out(#'inspect, __ALARM_TIME__ + 1);
}

void run(closure callback)
{
    done = callback;
    write_file("requests-clock", "2000000000\n");
    rm("request_owner.c");
    write_file("request_owner.c", "int submit(object source) { return update_blueprint(source, ({})); }\nmapping result(int id) { return update_blueprint_result(id); }\n");
    for (int i = 0; i < 8; i++) owners += ({clone_object("request_owner")});
    for (int i = 0; i < 65; i++)
    {
        string name = "requests_family" + i;
        rm(name + ".c"); write_file(name + ".c", "int state;\n");
        families += ({load_object(name)});
    }
    submit_round();
}
#endif
