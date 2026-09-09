#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, target, remote;
mixed held, alias;
int phase, request, checks;
string expected;

void require(int ok, string why)
{
    checks++;
    if (!ok) raise_error("Bindings: " + why + "\n");
}

void clean()
{
    set_driver_hook(H_MOVE_OBJECT0, 0);
    set_driver_hook(H_MOVE_OBJECT1, 0);
    held = alias = 0;
    if (target) destruct(target);
    if (remote) destruct(remote);
    if (blueprint) destruct(blueprint);
    rm("bindings_target.c");
}

void moved(object binding, object item)
{
    require(binding == (phase == 17 ? this_object() : item),
            "move hook runs with its specified binding");
}

void next();
void inspect()
{
    mapping report = update_blueprint_result(request);
    mixed err = catch(
        require(blueprint_outcome(report) == expected,
                sprintf("case %d expected %s, got %O", phase, expected, report["errors"])),
        require(report["matched"] == 1 && report["updated"] == (expected == "completed" && phase != 12 ? 1 : 0), "selection count and no migration"),
        require(report["already_current"] == (phase == 12), "only changing programs need compatibility"),
        require(sizeof(report["variable_changes"]) == (phase == 12 ? 0 : 1),
                "runtime rejection preserves schema diagnostics"),
        require(phase == 12 || report["variable_changes"][0]["removed"][0]["name"] == "removed",
                "runtime blocker retains removed-variable evidence"); publish);
    if (err) { clean(); funcall(done, 1); return; }
    msg("BINDING_CASE %d: %s\n", phase, expected);
    phase++;
    next();
}

void next()
{
    mixed err;
    clean();
    if (phase == 19)
    {
        msg("BLUEPRINT_BINDINGS: %d checks passed.\n", checks);
        funcall(done, 0);
        return;
    }
    err = catch(funcall(function void()
    {
        write_file("bindings_target.c", "#include \"target.inc\"\nint removed;\n");
        blueprint = load_object("bindings_target");
        target = clone_object(blueprint);
        remote = clone_object("remote");
        rm("bindings_target.c");
        write_file("bindings_target.c", "#include \"target.inc\"\n");
        expected = "LIVE_CLOSURE";
        switch (phase)
        {
            case 0: held = symbol_function("read_value", target); break;
            case 1: held = bind_lambda(target.handle(0), remote); break;
            case 2: held = bind_lambda(remote.handle(), target); break;
            case 3:
                held = bind_lambda(target.handle(5), remote);
                expected = "completed";
                break;
            case 4: case 5:
                held = target.handle(5); alias = held;
                held = bind_lambda(held, remote);
                if (phase == 5) { alias = 0; expected = "completed"; }
                break;
            case 6:
                held = bind_lambda(remote.handle(), target); alias = held;
                bind_lambda(held, remote);
                expected = "completed";
                break;
            case 7:
                held = bind_lambda(target.handle(0), remote);
                destruct(remote);
                expected = "completed";
                break;
            case 8:
                held = bind_lambda(remote.handle(), target);
                destruct(remote);
                expected = "completed";
                break;
            case 9:
                /* Diagnostic creator is the target program's blueprint;
                 * neither durable binding nor execution uses that program.
                 */
                held = target.foreign(remote);
                held = bind_lambda(held, remote);
                expected = "completed";
                break;
            case 10: held = bind_lambda(target.handle(0), new_lwobject("remote")); break;
            case 11: held = bind_lambda(new_lwobject("remote").handle(), target); break;
            case 12:
                held = ({blueprint.handle(0), target.handle(3)});
                expected = "completed";
                break;
            case 13:
                /* Old target migrates, but the source blueprint does not. */
                destruct(blueprint); blueprint = load_object("bindings_target");
                held = blueprint.handle(0);
                expected = "completed";
                break;
            case 14:
                destruct(blueprint); blueprint = load_object("bindings_target");
                held = target.handle(0);
                break;
            case 15:
                held = bind_lambda(target.foreign(remote), remote);
                destruct(blueprint); blueprint = load_object("bindings_target");
                expected = "completed";
                break;
            case 16:
                held = target.handle(0);
                write_file("bindings_target.c", "int unsupported = read_value();\n");
                expected = "SCHEMA_INCOMPATIBLE";
                break;
            case 17: case 18:
                set_driver_hook(phase == 17 ? H_MOVE_OBJECT0 : H_MOVE_OBJECT1,
                    unbound_lambda(({'item, 'dest}), ({#',,
                        ({#'set_environment, 'item, 'dest}),
                        ({#'call_other, this_object(), "moved", ({#'this_object}), 'item})
                    })));
                move_object(target, remote);
                expected = "completed";
                break;
        }
        request = phase >= 12 && phase <= 15 ? update_blueprint(blueprint, ({target}))
                              : update_blueprint("bindings_target", ({target}));
        call_out(#'inspect, __ALARM_TIME__ + 1);
    }); publish);
    if (err) { clean(); funcall(done, 1); }
}

void run(closure callback) { done = callback; next(); }
#endif
