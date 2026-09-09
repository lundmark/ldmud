#include "/inc/base.inc"

#ifdef __BLUEPRINT_UPDATE__
closure done;
object blueprint, first, second;
mixed *aliases, *other_aliases;
int request, checks;

void require(int ok, string label)
{
    checks++;
    if (!ok) raise_error("Aliases: " + label + "\n");
}

void clean()
{
    aliases = other_aliases = 0;
    if (first) destruct(first);
    if (second) destruct(second);
    if (blueprint) destruct(blueprint);
    rm("aliases_target.c");
    rm("migration-expect");
    rm("migration-observed");
}

void inspect()
{
    mixed err = catch(funcall(function void()
    {
        mapping report = update_blueprint_result(request);
        require(report["errors"][0]["code"] == "IMPLEMENTATION_INCOMPLETE",
                sprintf("alias preparation completes: %O", report["errors"]));
        require(read_file("migration-observed") == "3\n", "protected cells prepared");
        aliases[0] = 91;
        require(first.value() == 91 && second.value() == 83, "retained variable alias and clone isolation");
        aliases[1] = "XYZW";
        require(first.text_value() == "aXYZWef", "size-changing string range still attached");
        aliases[6] = 'q';
        require(first.text_value() == "aXYZWqf", "character alias tracks string range resizing");
        aliases[2] = ({8,9,10});
        require(sizeof(first.array_value()) == 5 && first.array_value()[1] == 8,
                "size-changing array range still attached");
        aliases[3] = 123;
        aliases[4] = ({20,21});
        require(first.map_value()["key",0] == 123 && first.map_value()["key",2] == 21,
                "mapping entry and range aliases preserved");
        aliases[5] = 72;
        require(first.removed_value() == 72, "removed cell still attached before publication");
        destruct(first);
        aliases[5] = 73;
        require(aliases[5] == 73, "removed cell survives detached from destroyed object");
        require(second.value() == 83 && other_aliases[0] == 83,
                "other clone aliases unchanged");
        msg("BLUEPRINT_ALIASES: %d checks passed.\n", checks);
    }); publish);
    clean();
    funcall(done, !!err);
}

void run(closure callback)
{
    done = callback;
    clean();
    write_file("aliases_target.c", "#pragma init_variables\n"
        "int retained; string text; mixed *array; mapping map; mixed removed; mixed *stored, *refarray;\n"
        "void seed(int n) { retained=n; text=\"abcdef\"; array=({0,1,2,3}); map=([\"key\":1;2;3]); removed=17; refarray=({&retained,&removed}); }\n"
        "mixed *aliases() { stored=({&retained,&(text[1..3]),&(array[1..2]),&(map[\"key\",0]),&(map[\"key\",1..2]),&removed,&(text[4]),&(map[\"later\",0]),&(map[\"missing\",0]),&(map[\"missing\",1..2]),&(refarray[0..1])}); map[\"later\",0]=31; return stored; }\n"
        "int value(){return retained;} string text_value(){return text;} mixed *array_value(){return array;} mapping map_value(){return map;} mixed removed_value(){return removed;}\n");
    blueprint = load_object("aliases_target");
    first = clone_object(blueprint); second = clone_object(blueprint);
    blueprint.seed(7); first.seed(41); second.seed(83);
    aliases = first.aliases(); other_aliases = second.aliases();
    rm("aliases_target.c");
    write_file("aliases_target.c", "#pragma init_variables\n"
        "mixed *stored, *refarray; mapping map; mixed *array; string text; int retained;\n");
    write_file("migration-expect", "3 1\n");
    request = update_blueprint("aliases_target", ({first, second}));
    call_out(#'inspect, __ALARM_TIME__ + 1);
}
#endif
