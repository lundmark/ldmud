#include "/inc/base.inc"
#include "/inc/blueprint.inc"
#include "/sys/driver_info.h"

/* This mudlib is launched explicitly by blueprint_stress.py. It is outside
 * t-* so that maximum-size cases do not run in every ordinary test suite. */
#ifndef STRESS_COUNT
#define STRESS_COUNT 100
#endif
#ifndef STRESS_WIDTH
#define STRESS_WIDTH 8
#endif
#ifndef STRESS_HANDLES
#define STRESS_HANDLES 0
#endif
#ifndef STRESS_CYCLES
#define STRESS_CYCLES 2
#endif
#ifndef STRESS_LATE_TARGET
#define STRESS_LATE_TARGET 0
#endif
#ifndef STRESS_CHURN
#define STRESS_CHURN 0
#endif
#ifndef STRESS_BATCH
#define STRESS_BATCH 1
#endif

object source;
object *cohort;
string *names;
mixed *handles, *temporary;
object *side_sources;
int *side_requests;
int request, generation = 1, installed = 1, checks, polls, attempts;
int before_memory, after_memory;

void require(int condition, string why)
{
    checks++;
    if (!condition) raise_error("Blueprint stress: " + why + "\n");
}

int memory_used() { return driver_info(DI_SIZE_MEMORY_USED); }

string side_program(int version)
{
    return sprintf("#pragma strong_types, save_types, init_variables\n"
                   "int counter = 17;\nint version() { return %d; }\n"
                   "int value() { return counter; }\n", version);
}

string program_source(int version)
{
    string text = "#pragma strong_types, save_types, init_variables\n";
    for (int j=0; j<STRESS_WIDTH; j++)
    {
        int index = version % 2 ? j : STRESS_WIDTH-1-j;
        text += sprintf("int v%d;\n", index);
    }
    text += sprintf("int extra_%d = 37;\n", version);
    if (version % 2 == 0)
        text += "int leading() { return -1; }\n";
    text += sprintf("int version() { return %d; }\n", version);
    text += "void seed(int index) {\n";
    for (int j=0; j<STRESS_WIDTH; j++)
        text += sprintf("v%d=index+%d;\n", j, j);
    text += "}\nint read_value() { return v0; }\n";
    text += "int *values() { return ({";
    for (int j=0; j<STRESS_WIDTH; j++)
        text += sprintf("v%d,", j);
    text += "}); }\n";
    text += "closure *handles() { return ({#'read_value,#'v0}); }\n";
    text += sprintf("int churn_fun_%d() { return extra_%d; }\n", version, version);
    text += sprintf("closure *temporary() { return ({#'churn_fun_%d,#'extra_%d}); }\n", version, version);
    return text;
}

void validate(int wanted)
{
    require(source.version() == wanted, "blueprint generation is atomic");
    for (int i=0; i<sizeof(cohort); i++)
    {
        object ob = cohort[i];
        require(ob && object_name(ob) == names[i], "clone identity survives");
        require(ob.version() == wanted, "whole clone cohort has expected behavior");
        int *values = ob.values();
        require(sizeof(values) == STRESS_WIDTH, "retained variable count");
        for (int j=0; j<STRESS_WIDTH; j++)
            require(values[j] == i+j, "every retained variable keeps its live value");
#if STRESS_HANDLES == 1
        require(funcall(handles[i][0]) == i && funcall(handles[i][1]) == i,
                "saved native handles follow reordered slots");
        closure *fresh = ob.handles();
        require(fresh[0] == handles[i][0] && fresh[1] == handles[i][1],
                "fresh native handles retain identity");
#endif
    }
#if STRESS_HANDLES == 2
    require(stress_python_check(wanted), "saved Python handles retain state and identity");
#endif
}

void submit_next();

void poll()
{
    if (catch(
        require(++polls < 120, "finite completion deadline"); publish))
    { shutdown(1); return; }
    mapping result = update_blueprint_result(request);
    if (result["status"] == "pending") { call_out(#'poll, 1); return; }
    for (int i=0; i<sizeof(side_requests); i++)
    {
        mapping side = update_blueprint_result(side_requests[i]);
        if (catch(
            require(side["status"] == "completed" && side["matched"] == 0
                    && side["updated"] == 0 && side["blueprint_updated"],
                    "independent source-only request in the same batch completes"),
            require(side_sources[i].version() == generation && side_sources[i].value() == 17,
                    "other batch requests preserve blueprint state"); publish))
        { shutdown(1); return; }
        msg("BLUEPRINT_STRESS_AUX_RESULT: id=%d batch=%d status=%s matched=%d updated=%d\n",
            side["id"], request, side["status"], side["matched"], side["updated"]);
    }
    if (catch(
        require(result["matched"] == sizeof(cohort), "report counts the full cohort"),
        require(result["status"] == "completed" || result["status"] == "failed",
                "request has terminal outcome"),
        require(result["updated"] == (result["status"] == "completed" ? sizeof(cohort) : 0),
                "successful full cohort or zero publication"),
        validate(result["status"] == "completed" ? generation : installed); publish))
    { shutdown(1); return; }
    msg("BLUEPRINT_STRESS_RESULT: id=%d count=%d width=%d handles=%d attempt=%d outcome=%s updated=%d checks=%d memory_used=%d batch=%d status=%s matched=%d\n",
        request, sizeof(cohort), STRESS_WIDTH, STRESS_HANDLES, attempts,
        blueprint_outcome(result), result["updated"], checks, memory_used(), request,
        result["status"], result["matched"]);
    if (result["status"] == "completed") installed = generation;
#if STRESS_CHURN
    if (result["status"] == "completed")
    {
        int before = memory_used();
        for (int i=0; i<sizeof(cohort); i++) temporary[i] = cohort[i].temporary();
        int live = memory_used();
        for (int i=0; i<sizeof(cohort); i++) temporary[i] = 0;
        int released = memory_used();
        msg("BLUEPRINT_STRESS_REGISTRY: generation=%d before=%d live=%d released=%d retained_delta=%d\n",
            installed, before, live, released, released-before);
    }
#endif
    if (result["status"] == "failed" || attempts == STRESS_CYCLES)
    {
        msg("BLUEPRINT_STRESS: %d checks, %d terminal attempts passed.\n", checks, attempts);
        shutdown(0);
    }
    else submit_next();
}

void submit_next()
{
    generation++;
    attempts++;
    write_file("cohort.c", program_source(generation), 1);
    request = update_blueprint("cohort");
    require(update_blueprint_result(request)["status"] == "pending", "submission defers execution");
    for (int i=0; i<sizeof(side_requests); i++)
    {
        string path = sprintf("side_%d", i);
        write_file(path + ".c", side_program(generation), 1);
        side_requests[i] = update_blueprint(path);
        require(update_blueprint_result(side_requests[i])["status"] == "pending",
                "independent request joins the next batch");
    }
#if STRESS_LATE_TARGET
    if (attempts == 1)
    {
        object extra = clone_object(source);
        extra.seed(sizeof(cohort));
        cohort += ({extra});
        names += ({object_name(extra)});
    }
#endif
    polls = 0;
    call_out(#'poll, 1);
}

void start()
{
    write_file("cohort.c", program_source(1), 1);
    source = load_object("cohort");
    cohort = allocate(STRESS_COUNT);
    names = allocate(STRESS_COUNT);
    handles = allocate(STRESS_COUNT);
    temporary = allocate(STRESS_COUNT);
    side_sources = allocate(STRESS_BATCH - 1);
    side_requests = allocate(STRESS_BATCH - 1);
    for (int i=0; i<sizeof(side_sources); i++)
    {
        string path = sprintf("side_%d", i);
        write_file(path + ".c", side_program(1), 1);
        side_sources[i] = load_object(path);
    }
    for (int i=0; i<STRESS_COUNT; i++)
    {
        cohort[i] = clone_object(source);
        cohort[i].seed(i);
        names[i] = object_name(cohort[i]);
    }
    before_memory = memory_used();
#if STRESS_HANDLES == 1
    for (int i=0; i<STRESS_COUNT; i++) handles[i] = cohort[i].handles();
#elif STRESS_HANDLES == 2
    require(stress_python_hold(cohort), "Python handle cohort created");
#endif
    after_memory = memory_used();
    msg("BLUEPRINT_STRESS_MEMORY: count=%d width=%d handles=%d before=%d held=%d delta=%d\n",
        STRESS_COUNT, STRESS_WIDTH, STRESS_HANDLES, before_memory, after_memory,
        after_memory-before_memory);
    validate(1);
    submit_next();
}

string *epilog(int flag)
{
    call_out(#'shutdown, 900, 1);
#ifdef __BLUEPRINT_UPDATE__
    if (catch(start(); publish)) shutdown(1);
#else
    msg("BLUEPRINT_STRESS: feature disabled.\n");
    shutdown(1);
#endif
    return 0;
}
