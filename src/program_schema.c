/* Immutable declaration identities and pure blueprint schema comparison.
 *
 * Slot offsets are locations, never identity. Normal inheritance introduces
 * a distinct edge path, while the compiler's virtual table anchors shared
 * storage. Mapped obsolete virtual layouts require a separate translation
 * proof and are rejected explicitly. Reports own only ordinary LPC values.
 */
#include "driver.h"
#ifdef USE_BLUEPRINT_UPDATE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "program_schema.h"
#include "array.h"
#include "exec.h"
#include "interpret.h"
#include "mapping.h"
#include "mstrings.h"
#include "structs.h"
#include "svalue.h"
#include "simulate.h"
#include "xalloc.h"

#define SCHEMA_MAX_DEPTH 64
#define SCHEMA_PATH_SIZE 2048
#define SCHEMA_MAX_WORK 1000000
#define SCHEMA_MAX_BLOCKERS 128
/* Covers a mapping, its scalar fields, names, bounded occurrence, and arrays. */
#define SCHEMA_RECORD_BYTES 8192
#define SCHEMA_VARIABLE_FLAGS (TYPE_MOD_PRIVATE | TYPE_MOD_PUBLIC \
    | TYPE_MOD_PROTECTED | TYPE_MOD_STATIC | TYPE_MOD_NO_MASK | VAR_INITIALIZED \
    | NAME_HIDDEN)
#define SCHEMA_FUNCTION_FLAGS (TYPE_MOD_PRIVATE | TYPE_MOD_PUBLIC \
    | TYPE_MOD_PROTECTED | TYPE_MOD_STATIC | TYPE_MOD_NO_MASK \
    | TYPE_MOD_VARARGS | TYPE_MOD_XVARARGS | TYPE_MOD_COROUTINE \
    | NAME_HIDDEN | NAME_UNDEFINED)

typedef struct schema_identity_s
{
    const program_t *program; /* Borrowed declaring program. */
    string_t *name;           /* Borrowed declaration name. */
    char path[SCHEMA_PATH_SIZE];
    Bool declared;
} schema_identity_t;

typedef struct schema_compare_s
{
    mapping_t *report;        /* Owned by the caller's rooted result. */
    vector_t *blockers;       /* Owned by report. */
    int num_blockers;
    unsigned int work;
} schema_compare_t;

/*-------------------------------------------------------------------------*/
static svalue_t *
field (mapping_t *map, const char *name)

/* Locate a writable field in a rooted mapping. Root the insertion key until
 * the mapping adopts it, including the allocation failure path.
 */
{
    svalue_t *value;
    push_c_string(inter_sp, name);
    value = get_map_lvalue(map, inter_sp);
    if (!value)
        outofmem(sizeof(*value), "blueprint schema field");
    pop_stack();
    return value;
} /* field() */

static mapping_t *
new_record (svalue_t *root)

/* Install an empty mapping immediately in its already rooted value slot. */
{
    mapping_t *map = allocate_mapping(8, 1);
    if (!map)
        outofmem(8, "blueprint schema record");
    put_mapping(root, map);
    return map;
} /* new_record() */

static void
canonical_name (svalue_t *root, string_t *name)

/* Program names have no leading slash and include .c; report load names
 * use one leading slash and omit .c. Never use object creation timestamps.
 */
{
    const char *text = get_txt(name);
    size_t len = mstrsize(name);
    while (len && *text == '/')
    {
        text++;
        len--;
    }
    if (len >= 2 && text[len-2] == '.' && text[len-1] == 'c')
        len -= 2;
    put_c_n_string(root, text, len);
    /* The caller's field roots the intermediate string during allocation. */
    string_t *canonical = add_slash(root->u.str);
    if (!canonical)
        outofmem(len + 1, "blueprint schema canonical name");
    free_svalue(root);
    put_string(root, canonical);
} /* canonical_name() */

static void
blocker (schema_compare_t *ctx, const char *code,
         const schema_identity_t *identity)

/* Keep bounded explanatory evidence. Exceeding the cap remains a blocker;
 * compatibility is never inferred from a truncated diagnostic list.
 */
{
    mapping_t *record;
    if (ctx->num_blockers == SCHEMA_MAX_BLOCKERS)
        return;
    record = new_record(&ctx->blockers->item[ctx->num_blockers++]);
    put_c_string(field(record, "code"), code);
    if (identity)
    {
        canonical_name(field(record, "program"), identity->program->name);
        put_ref_string(field(record, "name"), identity->name);
        put_c_string(field(record, "occurrence"), identity->path);
    }
} /* blocker() */

static Bool
spend_work (schema_compare_t *ctx)

/* Bound even quadratic matching and recursive identity discovery. */
{
    if (ctx->work++ < SCHEMA_MAX_WORK)
        return MY_TRUE;
    if (ctx->work == SCHEMA_MAX_WORK + 1)
        blocker(ctx, "SCHEMA_WORK_LIMIT", NULL);
    return MY_FALSE;
} /* spend_work() */

static Bool
edge_path (schema_identity_t *identity, const inherit_t *edge, int ordinal)

/* Append a direct normal-edge occurrence or reset to a shared virtual
 * anchor. A fixed-size path bounds compiler-generated inheritance depth.
 */
{
    size_t used = strlen(identity->path);
    int length;
    if (edge->inherit_type == INHERIT_TYPE_NORMAL)
        length = snprintf(identity->path + used, sizeof(identity->path) - used,
                          "/n%d", ordinal);
    else
    {
        const char *name = get_txt(edge->prog->name);
        size_t len = mstrsize(edge->prog->name);
        if (len >= 2 && name[len-2] == '.' && name[len-1] == 'c')
            len -= 2;
        used = 0;
        length = snprintf(identity->path, sizeof(identity->path), "v:/%.*s",
                          (int)len, name);
    }
    return length >= 0 && (size_t)length < sizeof(identity->path) - used;
} /* edge_path() */

static Bool
variable_identity (schema_compare_t *ctx, const program_t *prog, int slot,
                   schema_identity_t *identity, int depth)

/* Resolve a physical slot through the compiler's actual variable blocks.
 * Virtual subblocks occur first and are visited once. A nonvirtual edge's
 * offset is relative to the current nonvirtual block, while virtual offsets
 * address the complete block. Child indices resume after child virtual slots.
 */
{
    int ordinal = 0;
    if (!spend_work(ctx))
        return MY_FALSE;
    if (!prog->schema_generation || depth >= SCHEMA_MAX_DEPTH)
    {
        blocker(ctx, !prog->schema_generation ? "MISSING_SCHEMA_METADATA"
                                            : "SCHEMA_DEPTH_LIMIT", NULL);
        return MY_FALSE;
    }
    for (int i = 0; i < prog->num_inherited; i++)
    {
        const inherit_t *edge = &prog->inherit[i];
        int direct = ordinal;
        int size = edge->prog->num_variables - edge->prog->num_virtual_variables;
        int start = edge->variable_index_offset;
        Bool virtual_slot = slot < prog->num_virtual_variables;
        if (!spend_work(ctx))
            return MY_FALSE;
        if (edge->inherit_type == INHERIT_TYPE_NORMAL)
            start += prog->num_virtual_variables;
        if (edge->inherit_depth == 1)
            ordinal++;
        if (edge->inherit_mapped || edge->inherit_duplicate)
            continue;
        if (edge->inherit_type == INHERIT_TYPE_NORMAL && edge->inherit_depth != 1)
            continue;
        if ((edge->inherit_type != INHERIT_TYPE_NORMAL) != virtual_slot)
            continue;
        if (slot < start || slot >= start + size)
            continue;
        if (!edge_path(identity, edge, direct))
        {
            blocker(ctx, "SCHEMA_DEPTH_LIMIT", NULL);
            return MY_FALSE;
        }
        return variable_identity(ctx, edge->prog,
                    slot - start + edge->prog->num_virtual_variables,
                    identity, depth + 1);
    }
    identity->program = prog;
    identity->name = prog->variables[slot].name;
    identity->declared = prog->variables[slot].schema_declared;
    return MY_TRUE;
} /* variable_identity() */

static Bool
function_identity (schema_compare_t *ctx, const program_t *prog, int slot,
                   schema_identity_t *identity, int depth, function_t **header)

/* Preserve inherited dispatch paths. Cross-defined slots refer to the real
 * declaration, so their returned mapping may have multiple old handles.
 */
{
    funflag_t flags;
    if (!spend_work(ctx))
        return MY_FALSE;
    if (!prog->schema_generation || depth >= SCHEMA_MAX_DEPTH)
    {
        blocker(ctx, !prog->schema_generation ? "MISSING_SCHEMA_METADATA"
                                            : "SCHEMA_DEPTH_LIMIT", NULL);
        return MY_FALSE;
    }
    flags = prog->functions[slot];
    if (flags & NAME_CROSS_DEFINED)
    {
        slot += CROSSDEF_NAME_OFFSET(flags);
        flags = prog->functions[slot];
    }
    if (flags & NAME_INHERITED)
    {
        int index = flags & INHERIT_MASK;
        const inherit_t *edge = &prog->inherit[index];
        int ordinal = 0;
        for (int i = 0; i < index; i++)
        {
            if (!spend_work(ctx))
                return MY_FALSE;
            if (prog->inherit[i].inherit_depth == 1)
                ordinal++;
        }
        if (!edge_path(identity, edge, ordinal))
        {
            blocker(ctx, "SCHEMA_DEPTH_LIMIT", NULL);
            return MY_FALSE;
        }
        return function_identity(ctx, edge->prog, slot - edge->function_index_offset,
                                 identity, depth + 1, header);
    }
    *header = get_function_header(prog, slot);
    identity->program = prog;
    identity->name = (*header)->name;
    identity->declared = (*header)->schema_kind == SCHEMA_FUNCTION_NAMED;
    return MY_TRUE;
} /* function_identity() */

static Bool
same_identity (const schema_identity_t *left, const schema_identity_t *right)

/* Declaring path and name are stable across recompilation; program pointers
 * and slot offsets deliberately do not participate in declaration identity.
 */
{
    return mstreq(left->program->name, right->program->name)
        && mstreq(left->name, right->name) && !strcmp(left->path, right->path);
} /* same_identity() */

static void
declaration (svalue_t *root, const schema_identity_t *identity, int old, int next)

/* Describe both locations, using -1 for a missing side (addition/removal). */
{
    mapping_t *record = new_record(root);
    canonical_name(field(record, "program"), identity->program->name);
    put_ref_string(field(record, "name"), identity->name);
    put_c_string(field(record, "occurrence"), identity->path);
    put_number(field(record, "old_slot"), old);
    put_number(field(record, "new_slot"), next);
    put_number(field(record, "generated"), !identity->declared);
} /* declaration() */

static void
graph_blocker (schema_compare_t *ctx, const char *code,
               const program_t *old, const program_t *next, int index)

/* Identify the actual inheritance edge and generations that prevent a map.
 * A negative index denotes a whole-graph mismatch rather than a single edge.
 */
{
    int count = ctx->num_blockers;
    mapping_t *record;
    blocker(ctx, code, NULL);
    if (ctx->num_blockers == count)
        return;
    record = ctx->blockers->item[count].u.map;
    put_number(field(record, "inherit_index"), index);
    put_number(field(record, "old_inherit_count"), old->num_inherited);
    put_number(field(record, "candidate_inherit_count"), next->num_inherited);
    if (index >= 0)
    {
        const inherit_t *a = &old->inherit[index], *b = &next->inherit[index];
        canonical_name(field(record, "old_parent"), a->prog->name);
        canonical_name(field(record, "candidate_parent"), b->prog->name);
        put_number(field(record, "old_parent_generation"), a->prog->schema_generation);
        put_number(field(record, "candidate_parent_generation"), b->prog->schema_generation);
        put_number(field(record, "old_mapped"), a->inherit_mapped);
        put_number(field(record, "candidate_mapped"), b->inherit_mapped);
    }
} /* graph_blocker() */

static Bool
graph_compatible (schema_compare_t *ctx, const program_t *old, const program_t *next)

/* Identical parent generations imply identical recursive graphs. Compare
 * explicit order, kinds, visibility and virtual sharing, ignoring offsets.
 */
{
    if (!old->schema_generation || !next->schema_generation)
    {
        blocker(ctx, "MISSING_SCHEMA_METADATA", NULL);
        return MY_FALSE;
    }
    if (!mstreq(old->name, next->name) || old->num_inherited != next->num_inherited)
    {
        graph_blocker(ctx, "INHERITANCE_CHANGED", old, next, -1);
        return MY_FALSE;
    }
    for (int i = 0; i < old->num_inherited; i++)
    {
        const inherit_t *a = &old->inherit[i], *b = &next->inherit[i];
        if (!spend_work(ctx))
            return MY_FALSE;
        if (a->inherit_mapped || b->inherit_mapped)
        {
            graph_blocker(ctx, "MAPPED_INHERITANCE_UNSUPPORTED", old, next, i);
            return MY_FALSE;
        }
        if (a->prog != b->prog || a->inherit_type != b->inherit_type
         || a->inherit_depth != b->inherit_depth || a->inherit_duplicate != b->inherit_duplicate
         || a->inherit_hidden != b->inherit_hidden || a->inherit_public != b->inherit_public)
        {
            graph_blocker(ctx, "INHERITANCE_CHANGED", old, next, i);
            return MY_FALSE;
        }
    }
    return MY_TRUE;
} /* graph_compatible() */

static Bool
struct_compatible (schema_compare_t *ctx, const struct_type_t *a,
                   const struct_type_t *b, int depth)

/* Interned LPC type pointers identify a struct name, not its member layout.
 * Compare retained concrete definitions, including bases and member order.
 */
{
    if (!spend_work(ctx) || depth >= SCHEMA_MAX_DEPTH)
        return MY_FALSE;
    if (!a || !b)
        return a == b;
    if (a->name != b->name || a->num_members != b->num_members)
        return MY_FALSE;
    if (!struct_compatible(ctx, a->base, b->base, depth + 1))
        return MY_FALSE;
    for (int i = 0; i < a->num_members; i++)
    {
        if (!spend_work(ctx))
            return MY_FALSE;
        if (!mstreq(a->member[i].name, b->member[i].name)
         || a->member[i].type != b->member[i].type)
            return MY_FALSE;
    }
    return MY_TRUE;
} /* struct_compatible() */

static void
trim (svalue_t *root, int count)

/* Replace a rooted oversized result array after bounded construction. */
{
    vector_t *array = slice_array(root->u.vec, 0, count - 1);
    free_svalue(root);
    put_array(root, array);
} /* trim() */

static void
compare_variables (schema_compare_t *ctx, program_t *old, program_t *next)

/* Match source declarations; generated slots remain explicitly unmatchable.
 * All partial summaries are rooted in ctx->report before further allocation.
 */
{
    svalue_t *matched = field(ctx->report, "matched");
    svalue_t *added = field(ctx->report, "added");
    svalue_t *removed = field(ctx->report, "removed");
    int matches = 0, additions = 0, removals = 0;
    put_array(matched, allocate_array(old->num_variables));
    put_array(removed, allocate_array(old->num_variables));
    put_array(added, allocate_array(next->num_variables));
    for (int side = 0; side < 2; side++)
    {
        program_t *a = side ? next : old, *b = side ? old : next;
        for (int i = 0; i < a->num_variables; i++)
        {
            schema_identity_t id = { .path = "$" };
            int j;
            if (!variable_identity(ctx, a, i, &id, 0))
                goto finish;
            if (!id.declared)
            {
                blocker(ctx, "UNMATCHABLE_VARIABLE", &id);
                continue;
            }
            for (j = 0; j < b->num_variables; j++)
            {
                schema_identity_t other = { .path = "$" };
                if (!variable_identity(ctx, b, j, &other, 0))
                    goto finish;
                if (other.declared && same_identity(&id, &other))
                    break;
            }
            if (j == b->num_variables)
            {
                if (side)
                    declaration(&added->u.vec->item[additions++], &id, -1, i);
                else
                    declaration(&removed->u.vec->item[removals++], &id, i, -1);
            }
            else if (!side)
            {
                declaration(&matched->u.vec->item[matches++], &id, i, j);
                if (a->variables[i].type.t_type != b->variables[j].type.t_type)
                    blocker(ctx, "VARIABLE_TYPE_CHANGED", &id);
                if ((a->variables[i].type.t_flags ^ b->variables[j].type.t_flags)
                    & SCHEMA_VARIABLE_FLAGS)
                    blocker(ctx, "VARIABLE_MODIFIERS_CHANGED", &id);
            }
        }
    }
finish:
    trim(matched, matches);
    trim(added, additions);
    trim(removed, removals);
} /* compare_variables() */

static Bool
signature_available (const program_t *prog, const function_t *head)

/* Never fall back to arity-only evidence, even if save_types is absent. */
{
    return head->schema_argument_start != UINT_MAX
        && head->schema_argument_start <= prog->num_schema_arguments
        && head->num_arg <= prog->num_schema_arguments - head->schema_argument_start;
} /* signature_available() */

static Bool
signature_equal (schema_compare_t *ctx, const program_t *a, const function_t *ah,
                 const program_t *b, const function_t *bh)

/* Require exact declared return, argument, reference and optional contracts. */
{
    if (ah->type != bh->type || ah->num_arg != bh->num_arg
     || ah->num_opt_arg != bh->num_opt_arg)
        return MY_FALSE;
    for (int i = 0; i < ah->num_arg; i++)
    {
        const struct schema_argument_s *at = &a->schema_arguments[ah->schema_argument_start+i];
        const struct schema_argument_s *bt = &b->schema_arguments[bh->schema_argument_start+i];
        if (!spend_work(ctx))
            return MY_FALSE;
        if (a->types[at->type_index] != b->types[bt->type_index] || at->flags != bt->flags)
            return MY_FALSE;
    }
    return MY_TRUE;
} /* signature_equal() */

static void
compare_functions (schema_compare_t *ctx, program_t *old, program_t *next)

/* Describe named and generated handles separately. Removed named functions
 * are represented by new_slot=-1.
 */
{
    svalue_t *functions = field(ctx->report, "functions");
    int count = 0;
    put_array(functions, allocate_array(old->num_functions));
    for (int i = 0; i < old->num_functions; i++)
    {
        schema_identity_t id = { .path = "$" };
        function_t *head;
        int j;
        if (!function_identity(ctx, old, i, &id, 0, &head))
            break;
        if (!id.declared)
        {
            declaration(&functions->u.vec->item[count++], &id, i, -1);
            continue;
        }
        if (!signature_available(id.program, head))
            blocker(ctx, "MISSING_SCHEMA_METADATA", &id);
        for (j = 0; j < next->num_functions; j++)
        {
            schema_identity_t other = { .path = "$" };
            function_t *new_head;
            if (!function_identity(ctx, next, j, &other, 0, &new_head))
                goto finish;
            if (!other.declared || !same_identity(&id, &other)
             || ((old->functions[i] ^ next->functions[j]) & NAME_CROSS_DEFINED))
                continue;
            if (!signature_available(other.program, new_head))
                blocker(ctx, "MISSING_SCHEMA_METADATA", &other);
            else if (signature_available(id.program, head)
                  && !signature_equal(ctx, id.program, head, other.program, new_head))
            {
                if (ctx->work > SCHEMA_MAX_WORK)
                    goto finish;
                blocker(ctx, "FUNCTION_SIGNATURE_CHANGED", &id);
            }
            if ((old->schema_function_flags[i] ^ next->schema_function_flags[j])
                & SCHEMA_FUNCTION_FLAGS)
                blocker(ctx, "FUNCTION_MODIFIERS_CHANGED", &id);
            break;
        }
        declaration(&functions->u.vec->item[count++], &id, i,
                    j == next->num_functions ? -1 : j);
    }
finish:
    trim(functions, count);
} /* compare_functions() */

void
program_schema_compare (program_t *old, program_t *candidate, svalue_t *result,
                        size_t *remaining)

/* Build a nonmutating map and blockers for one actual old generation.
 * The caller owns both program pins and the rooted zero result. No program
 * pointers escape: cached reports survive program retirement independently.
 */
{
    schema_compare_t ctx;
    svalue_t *blocks;
    size_t records = (size_t)old->num_variables + candidate->num_variables
                     + old->num_functions + SCHEMA_MAX_BLOCKERS + 1;
    /* Keep one record unit in reserve. Zero is exclusively the explicit
     * memory-limit sentinel used by the request to stop further generation
     * discovery; successful exact exhaustion must never silently truncate it.
     */
    Bool within_budget = records < *remaining / SCHEMA_RECORD_BYTES;
    if (within_budget)
        *remaining -= records * SCHEMA_RECORD_BYTES;
    else
        *remaining = 0;
    ctx.report = new_record(result);
    ctx.num_blockers = 0;
    ctx.work = 0;
    put_number(field(ctx.report, "old_generation"), old->schema_generation);
    put_number(field(ctx.report, "candidate_generation"), candidate->schema_generation);
    blocks = field(ctx.report, "blockers");
    put_array(blocks, allocate_array_unlimited(SCHEMA_MAX_BLOCKERS));
    ctx.blockers = blocks->u.vec;
    if (!within_budget)
        blocker(&ctx, "SCHEMA_MEMORY_LIMIT", NULL);
    if (within_budget && graph_compatible(&ctx, old, candidate))
    {
        compare_variables(&ctx, old, candidate);
        compare_functions(&ctx, old, candidate);
        for (int i = 0; i < old->num_structs; i++)
            for (int j = 0; j < candidate->num_structs; j++)
            {
                if (!spend_work(&ctx))
                    goto finish;
                if (old->struct_defs[i].type->name == candidate->struct_defs[j].type->name)
                {
                    Bool compatible = struct_compatible(&ctx, old->struct_defs[i].type,
                                                        candidate->struct_defs[j].type, 0);
                    if (ctx.work > SCHEMA_MAX_WORK)
                        goto finish;
                    if (!compatible || old->struct_defs[i].flags != candidate->struct_defs[j].flags)
                        blocker(&ctx, "STRUCT_LAYOUT_CHANGED", NULL);
                }
            }
    }
    else
    {
        put_array(field(ctx.report, "matched"), allocate_array(0));
        put_array(field(ctx.report, "added"), allocate_array(0));
        put_array(field(ctx.report, "removed"), allocate_array(0));
        put_array(field(ctx.report, "functions"), allocate_array(0));
    }
finish:
    trim(blocks, ctx.num_blockers);
} /* program_schema_compare() */

#ifdef DEBUG
void
program_schema_check (const program_t *prog)

/* Schema tables own no separate references: existing types/name walkers
 * own every referenced value. Validate embedded offsets before ref checks.
 */
{
    assert(prog->schema_generation > 0);
    assert((char *)prog->schema_function_flags >= (char *)prog);
    assert((char *)(prog->schema_function_flags + prog->num_functions)
           <= (char *)prog + prog->total_size);
    if (prog->num_schema_arguments)
    {
        assert((char *)prog->schema_arguments >= (char *)prog);
        assert((char *)(prog->schema_arguments + prog->num_schema_arguments)
               <= (char *)prog + prog->total_size);
    }
    for (unsigned int i = 0; i < prog->num_schema_arguments; i++)
        assert(prog->schema_arguments[i].type_index < prog->num_types);
} /* program_schema_check() */
#endif
#endif /* USE_BLUEPRINT_UPDATE */
