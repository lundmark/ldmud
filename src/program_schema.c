/* Immutable declaration identities and pure blueprint schema comparison.
 *
 * Slot offsets are locations, never identity. Normal inheritance introduces
 * a distinct edge path, while the compiler's virtual table anchors shared
 * storage. Mapped obsolete virtual layouts require a separate translation
 * proof and are rejected explicitly. Reports own only ordinary LPC values.
 */
#include "driver.h"
#include "program_update.h"
#ifdef USE_BLUEPRINT_UPDATE
#include <assert.h>
#include <math.h>
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
#define SCHEMA_MAX_BLOCKERS 128
/* Covers a mapping, its scalar fields, names, bounded occurrence, and arrays. */
#define SCHEMA_RECORD_BYTES 8192
/* Bounds the private mapping chain's next pointer, key, and alignment. */
#define SCHEMA_CHAIN_HEADER (sizeof(void *) + 2 * sizeof(svalue_t))
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
    int slot;                /* Resolved slot in the declaring program. */
} schema_identity_t;

typedef struct schema_compare_s
{
    mapping_t *report;        /* Owned by the caller's rooted result. */
    vector_t *blockers;       /* Owned by report. */
    int num_blockers;
    Bool work_exhausted;
    schema_budget_t *budget;
    vector_t *prepared;       /* Request-rooted literal source additions only. */
    Bool preparing_blueprint;
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
    if (stack_gap_guard_failed())
        errorf("update_blueprint(): memory pressure preparing schema field.\n");
    value = get_map_lvalue(map, inter_sp);
    if (!value)
        outofmem(sizeof(*value), "blueprint schema field");
    pop_stack();
    if (stack_gap_guard_failed())
        errorf("update_blueprint(): memory pressure inserting schema field.\n");
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
    if (stack_gap_guard_failed())
        errorf("update_blueprint(): memory pressure preparing schema record.\n");
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
    if (stack_gap_guard_failed())
        errorf("update_blueprint(): memory pressure describing schema name.\n");
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
    if (ctx->budget->work)
    {
        ctx->budget->work--;
        return MY_TRUE;
    }
    if (!ctx->work_exhausted)
        blocker(ctx, "SCHEMA_WORK_LIMIT", NULL);
    ctx->work_exhausted = MY_TRUE;
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
    identity->slot = slot;
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

static Bool
default_range (const program_t *prog, uint32_t offset, uint32_t count,
               size_t element)

{
    return offset <= prog->schema_defaults_size
        && count <= (prog->schema_defaults_size - offset) / element;
}

static const struct schema_defaults_s *
default_header (const program_t *prog)

/* Validate the complete embedded directory before deriving any pointers. */

{
    const struct schema_defaults_s *header;
    uintptr_t base = (uintptr_t)prog, address = (uintptr_t)prog->schema_defaults;
    size_t offset;
    if (!prog->schema_defaults || prog->total_size < 0 || address < base)
        return NULL;
    offset = address - base;
    if (offset < sizeof(*prog) || offset > (size_t)prog->total_size
     || address % sizeof(void *)
     || prog->schema_defaults_size > (size_t)prog->total_size - offset
     || prog->schema_defaults_size < sizeof(*header))
        return NULL;
    header = (const struct schema_defaults_s *)prog->schema_defaults;
    if (header->version != SCHEMA_DEFAULT_VERSION
     || !default_range(prog, header->record_offset, header->records,
                        sizeof(struct schema_default_s))
     || !default_range(prog, header->node_offset, header->nodes,
                        sizeof(struct schema_default_node_s))
     || !default_range(prog, header->edge_offset, header->edges, sizeof(uint32_t))
     || !default_range(prog, header->byte_offset, header->bytes, 1)
     || header->record_offset % sizeof(void *)
     || header->node_offset % sizeof(void *)
     || header->edge_offset % sizeof(void *)
     || header->byte_offset % sizeof(void *)
     || header->record_offset < sizeof(*header)
     || header->node_offset < header->record_offset
                         + (size_t)header->records * sizeof(struct schema_default_s)
     || header->edge_offset < header->node_offset
                         + (size_t)header->nodes * sizeof(struct schema_default_node_s)
     || header->byte_offset < header->edge_offset
                         + (size_t)header->edges * sizeof(uint32_t))
        return NULL;
    return header;
}

static Bool
default_bytes (schema_compare_t *ctx, size_t bytes)

{
    if (bytes >= ctx->budget->bytes)
        return MY_FALSE;
    ctx->budget->bytes -= bytes;
    return MY_TRUE;
}

static Bool
default_node_valid (const program_t *owner, const struct schema_defaults_s *header,
                    uint32_t index)

/* Validate one needed node without recursive descent. Postorder IDs and the
 * exact derived depth bound both malformed metadata and later traversal.
 */
{
    const struct schema_default_node_s *nodes =
        (const struct schema_default_node_s *)(owner->schema_defaults + header->node_offset);
    const struct schema_default_node_s *node;
    const uint32_t *edges;
    uint32_t depth = 1;
    if (index >= header->nodes)
        return MY_FALSE;
    node = &nodes[index];
    if (node->edge_start > header->edges
     || node->edge_count > header->edges - node->edge_start
     || !node->depth || node->depth > BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH
     || node->kind > SCHEMA_DEFAULT_MAPPING)
        return MY_FALSE;
    if (node->kind < SCHEMA_DEFAULT_ARRAY)
    {
        if (node->edge_count || node->width || node->depth != 1)
            return MY_FALSE;
        if (node->kind == SCHEMA_DEFAULT_FLOAT && !isfinite(node->value.floating))
            return MY_FALSE;
    }
    if (node->kind == SCHEMA_DEFAULT_STRING || node->kind == SCHEMA_DEFAULT_BYTES)
    {
        if (node->text_start > header->bytes
         || node->text_size > header->bytes - node->text_start
         || node->unicode > STRING_BYTES
         || (node->kind == SCHEMA_DEFAULT_BYTES) != (node->unicode == STRING_BYTES))
            return MY_FALSE;
    }
    else if (node->text_start || node->text_size || node->unicode)
        return MY_FALSE;
    if (node->kind == SCHEMA_DEFAULT_ARRAY && node->width)
        return MY_FALSE;
    if (node->kind == SCHEMA_DEFAULT_MAPPING
     && (node->width < 0
      || (p_uint)node->width > (SSIZE_MAX - SCHEMA_CHAIN_HEADER) / sizeof(svalue_t)
      || node->edge_count % ((size_t)node->width + 1)))
        return MY_FALSE;
    edges = (const uint32_t *)(owner->schema_defaults + header->edge_offset) + node->edge_start;
    for (uint32_t i = 0; i < node->edge_count; i++)
    {
        if (edges[i] >= index || nodes[edges[i]].depth >= BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH)
            return MY_FALSE;
        if (depth <= nodes[edges[i]].depth)
            depth = nodes[edges[i]].depth + 1;
    }
    return node->depth == depth;
}

static Bool
default_record_valid (const struct schema_defaults_s *header,
                      const struct schema_default_s *record)
{
    return record->status <= SCHEMA_DEFAULT_UNAVAILABLE
        && record->reason <= SCHEMA_DEFAULT_REASON_METADATA
        && !(record->flags & ~SCHEMA_DEFAULT_RTT_CHECK)
        && record->source_start <= header->bytes
        && record->source_size <= header->bytes - record->source_start
        && (record->status != SCHEMA_DEFAULT_SUPPORTED || record->root < header->nodes)
        && (record->status > SCHEMA_DEFAULT_SUPPORTED || !record->reason);
}

static Bool
materialize_default (schema_compare_t *ctx, const program_t *owner,
                     const struct schema_defaults_s *header,
                     uint32_t index, svalue_t *root, unsigned int depth)

/* The destination belongs to the rooted report before any allocation. */

{
    const struct schema_default_node_s *node;
    const uint32_t *edges;
    string_t *str;
    if (index >= header->nodes || depth >= BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH
     || !spend_work(ctx))
        return MY_FALSE;
    if (!ctx->budget->slots)
        return MY_FALSE;
    ctx->budget->slots--;
    node = (const struct schema_default_node_s *)(owner->schema_defaults + header->node_offset) + index;
    if (node->edge_count > ctx->budget->work)
    {
        ctx->budget->work = 0;
        return MY_FALSE;
    }
    ctx->budget->work -= node->edge_count;
    if (!default_node_valid(owner, header, index))
        return MY_FALSE;
    edges = (const uint32_t *)(owner->schema_defaults + header->edge_offset) + node->edge_start;
    switch (node->kind)
    {
        case SCHEMA_DEFAULT_INTEGER:
            put_number(root, node->value.integer);
            break;
        case SCHEMA_DEFAULT_FLOAT:
            if (!isfinite(node->value.floating))
                return MY_FALSE;
            put_float(root, node->value.floating);
            break;
        case SCHEMA_DEFAULT_STRING:
        case SCHEMA_DEFAULT_BYTES:
            if (node->text_start > header->bytes
             || node->text_size > header->bytes - node->text_start
             || node->unicode > STRING_BYTES
             || (node->kind == SCHEMA_DEFAULT_BYTES) != (node->unicode == STRING_BYTES)
             || !default_bytes(ctx, sizeof(string_t) + (size_t)node->text_size + 1))
                return MY_FALSE;
            str = new_n_mstring((const char *)owner->schema_defaults
                                  + header->byte_offset + node->text_start,
                                node->text_size, node->unicode);
            if (!str)
                outofmem(node->text_size, "blueprint default string");
            if (node->kind == SCHEMA_DEFAULT_BYTES)
                put_bytes(root, str);
            else
                put_string(root, str);
            DEFAULT_TEST_PRESSURE(DEFAULT_TEST_STRING_PRESSURE);
            break;
        case SCHEMA_DEFAULT_ARRAY:
        {
            vector_t *array;
            if ((max_array_size && node->edge_count > max_array_size)
             || (size_t)node->edge_count > (SSIZE_MAX - sizeof(vector_t)) / sizeof(svalue_t)
             || !default_bytes(ctx, sizeof(vector_t) + (size_t)node->edge_count * sizeof(svalue_t)))
                return MY_FALSE;
            array = allocate_array(node->edge_count);
            if (!array)
                outofmem(node->edge_count, "blueprint default array");
            put_array(root, array);
            DEFAULT_TEST_PRESSURE(DEFAULT_TEST_ARRAY_PRESSURE);
            if (stack_gap_guard_failed())
                errorf("update_blueprint(): memory pressure creating default array.\n");
            for (uint32_t i = 0; i < node->edge_count; i++)
                if (edges[i] >= index
                 || !materialize_default(ctx, owner, header, edges[i], &array->item[i], depth + 1))
                    return MY_FALSE;
            break;
        }
        case SCHEMA_DEFAULT_MAPPING:
        {
            mapping_t *mapping;
            size_t rows, stride, row_bytes, bytes;
            if (node->width < 0
             || (p_uint)node->width > (SSIZE_MAX - SCHEMA_CHAIN_HEADER) / sizeof(svalue_t))
                return MY_FALSE;
            stride = (size_t)node->width + 1;
            if (node->edge_count % stride)
                return MY_FALSE;
            rows = node->edge_count / stride;
            if ((max_mapping_size && node->edge_count > max_mapping_size)
             || (max_mapping_keys && rows > max_mapping_keys))
                return MY_FALSE;
            row_bytes = SCHEMA_CHAIN_HEADER + (size_t)node->width * sizeof(svalue_t) + sizeof(void *);
            bytes = sizeof(mapping_t) + (rows ? sizeof(mapping_hash_t) : 0);
            if (rows > (SIZE_MAX - bytes) / row_bytes
             || !default_bytes(ctx, bytes + rows * row_bytes))
                return MY_FALSE;
            mapping = allocate_mapping(rows, node->width);
            if (!mapping)
                outofmem(bytes, "blueprint default mapping");
            put_mapping(root, mapping);
            DEFAULT_TEST_PRESSURE(DEFAULT_TEST_MAPPING_PRESSURE);
            if (stack_gap_guard_failed())
                errorf("update_blueprint(): memory pressure creating default mapping.\n");
            for (size_t i = 0; i < rows; i++)
            {
                svalue_t *values;
                uint32_t key = edges[i * stride];
                p_int old_entries = mapping->num_entries;
                Bool duplicate;
                /* Only the temporary key needs another owner. Value slots
                 * already belong to the rooted mapping, including overwrite.
                 */
                push_number(inter_sp, 0);
                if (key >= index
                 || !materialize_default(ctx, owner, header, key, inter_sp, depth + 1))
                {
                    pop_stack();
                    return MY_FALSE;
                }
                values = get_map_lvalue_unchecked(mapping, inter_sp);
                if (!values)
                    outofmem(row_bytes, "blueprint default mapping row");
                pop_stack();
                duplicate = old_entries == mapping->num_entries;
                DEFAULT_TEST_PRESSURE(DEFAULT_TEST_INSERT_PRESSURE);
                if (stack_gap_guard_failed())
                    errorf("update_blueprint(): memory pressure inserting default row.\n");
                for (size_t j = 1; j < stride; j++)
                {
                    uint32_t child = edges[i * stride + j];
                    free_svalue(&values[j-1]);
                    put_number(&values[j-1], 0);
                    if (duplicate)
                        DEFAULT_TEST_PRESSURE(DEFAULT_TEST_DUPLICATE_RELEASE);
                    if (j == 2)
                        DEFAULT_TEST_PRESSURE(DEFAULT_TEST_ROW_CHILD);
                    if (stack_gap_guard_failed())
                        errorf("update_blueprint(): memory pressure releasing default value.\n");
                    if (child >= index
                     || !materialize_default(ctx, owner, header, child, &values[j-1], depth + 1))
                        return MY_FALSE;
                    if (duplicate)
                        DEFAULT_TEST_PRESSURE(DEFAULT_TEST_DUPLICATE_CHILD);
                    if (stack_gap_guard_failed())
                        errorf("update_blueprint(): memory pressure replacing default value.\n");
                }
            }
            break;
        }
        default:
            return MY_FALSE;
    }
    if (stack_gap_guard_failed())
        errorf("update_blueprint(): memory pressure materializing defaults.\n");
    return MY_TRUE;
}

static Bool
materialize_requested_default (schema_compare_t *ctx, const program_t *owner,
                               const struct schema_defaults_s *header,
                               uint32_t index, svalue_t *root)
{
    Bool success;
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    program_update_default_test_scope(MY_TRUE);
#endif
    success = materialize_default(ctx, owner, header, index, root, 0);
#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
    program_update_default_test_scope(MY_FALSE);
#endif
    return success;
}

static Bool
describe_default (schema_compare_t *ctx, const program_t *candidate, int slot,
                  const schema_identity_t *identity, svalue_t *declaration_root)

{
    const program_t *owner = identity->program;
    const struct schema_defaults_s *header;
    const struct schema_default_s *description;
    uint32_t index = owner->variables[identity->slot].schema_default;
    mapping_t *report = new_record(field(declaration_root->u.map, "default"));
    svalue_t *value;

    /* Arbitrary shared values may contain objects or closures. Only describe
     * the copy decision; terminal evidence must not retain those references.
     */
    if (!ctx->preparing_blueprint
     && !(candidate->variables[slot].type.t_flags & VAR_INITIALIZED)
     && (candidate->blueprint || ctx->prepared))
    {
        put_c_string(field(report, "kind"), "shared");
        return MY_TRUE;
    }
    header = default_header(owner);
    if (!header || !index || index > header->records)
    {
        put_c_string(field(report, "kind"), "missing");
        blocker(ctx, "MISSING_DEFAULT_METADATA", identity);
        return MY_FALSE;
    }
    description = (const struct schema_default_s *)(owner->schema_defaults + header->record_offset) + index - 1;
    if (!default_record_valid(header, description)
     || !default_bytes(ctx, sizeof(string_t) + (size_t)description->source_size + 1))
    {
        put_c_string(field(report, "kind"), "unavailable");
        blocker(ctx, "INVALID_DEFAULT_METADATA", identity);
        return MY_FALSE;
    }
    put_number(field(report, "line"), description->line);
    if (description->source_start <= header->bytes
     && description->source_size <= header->bytes - description->source_start)
        put_c_n_string(field(report, "file"),
                        (const char *)owner->schema_defaults + header->byte_offset + description->source_start,
                        description->source_size);
    if (description->status == SCHEMA_DEFAULT_UNSUPPORTED
     || description->status == SCHEMA_DEFAULT_UNAVAILABLE
     || description->status == SCHEMA_DEFAULT_MISSING)
    {
        put_c_string(field(report, "kind"), description->status == SCHEMA_DEFAULT_UNSUPPORTED
                                          ? "unsupported" : "unavailable");
        put_number(field(report, "reason"), description->reason);
        blocker(ctx, "UNSUPPORTED_DEFAULT", identity);
        return MY_FALSE;
    }
    value = field(report, "value");
    if (description->status == SCHEMA_DEFAULT_INT_ZERO)
        put_number(value, 0);
    else if (description->status == SCHEMA_DEFAULT_FLOAT_ZERO)
        put_float(value, 0.0);
    else if (description->status != SCHEMA_DEFAULT_SUPPORTED
          || !materialize_requested_default(ctx, owner, header, description->root, value))
    {
        free_svalue(value);
        put_number(value, 0);
        put_c_string(field(report, "kind"), "unavailable");
        blocker(ctx, "DEFAULT_MATERIALIZATION_LIMIT", identity);
        return MY_FALSE;
    }
    put_c_string(field(report, "kind"), description->status == SCHEMA_DEFAULT_SUPPORTED
                                      ? "literal" : "implicit");
    if (description->flags & SCHEMA_DEFAULT_RTT_CHECK)
    {
        Bool exhausted;
        Bool compatible = check_rtt_compatibility_bounded(
            owner->variables[identity->slot].type.t_type, value,
            &ctx->budget->work, &exhausted);
        if (exhausted)
        {
            blocker(ctx, "DEFAULT_TYPE_WORK_LIMIT", identity);
            return MY_FALSE;
        }
        else if (!compatible)
        {
            if (owner->flags & P_WARN_RTT_CHECKS)
                put_number(field(report, "type_warning"), 1);
            else
            {
                blocker(ctx, "DEFAULT_TYPE_MISMATCH", identity);
                return MY_FALSE;
            }
        }
    }
    return MY_TRUE;
}

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

static Bool
compare_variables (schema_compare_t *ctx, program_t *old, program_t *next)

/* Match source declarations; generated slots remain explicitly unmatchable.
 * All partial summaries are rooted in ctx->report before further allocation.
 */
{
    svalue_t *matched = field(ctx->report, "matched");
    svalue_t *added = field(ctx->report, "added");
    svalue_t *removed = field(ctx->report, "removed");
    int matches = 0, additions = 0, removals = 0;
    Bool defaults_ready = MY_TRUE;
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
            {
                defaults_ready = MY_FALSE;
                goto finish;
            }
            if (!id.declared)
            {
                defaults_ready = MY_FALSE;
                blocker(ctx, "UNMATCHABLE_VARIABLE", &id);
                continue;
            }
            for (j = 0; j < b->num_variables; j++)
            {
                schema_identity_t other = { .path = "$" };
                if (!variable_identity(ctx, b, j, &other, 0))
                {
                    defaults_ready = MY_FALSE;
                    goto finish;
                }
                if (other.declared && same_identity(&id, &other))
                    break;
            }
            if (j == b->num_variables)
            {
                if (side)
                {
                    svalue_t *entry = &added->u.vec->item[additions++];
                    declaration(entry, &id, -1, i);
                    if (!describe_default(ctx, next, i, &id, entry))
                        defaults_ready = MY_FALSE;
                    if (ctx->preparing_blueprint)
                    {
                        svalue_t *description = field(entry->u.map, "default");
                        svalue_t *value = field(description->u.map, "value");
                        /* Only literal/implicit additions enter this scratch
                         * root. Retained live values remain on the source.
                         */
                        assign_svalue_no_free(&ctx->prepared->item[i], value);
                    }
                }
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
    return defaults_ready;
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
                if (ctx->work_exhausted)
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

Bool
program_schema_compare (program_t *old, program_t *candidate, svalue_t *result,
                        schema_budget_t *budget, vector_t *prepared, Bool preparing_blueprint)

/* Build a nonmutating map and blockers for one actual old generation.
 * The caller owns both program pins and the rooted zero result. No program
 * pointers escape: cached reports survive program retirement independently.
 */
{
    schema_compare_t ctx;
    svalue_t *blocks;
    Bool defaults_ready = MY_FALSE;
    size_t records = (size_t)old->num_variables + candidate->num_variables
                     + old->num_functions + SCHEMA_MAX_BLOCKERS + 1;
    /* Keep one record unit in reserve. Zero is exclusively the explicit
     * memory-limit sentinel used by the request to stop further generation
     * discovery; successful exact exhaustion must never silently truncate it.
     */
    size_t slots = (size_t)old->num_variables + candidate->num_variables;
    Bool within_budget = records < budget->bytes / SCHEMA_RECORD_BYTES
                         && slots <= budget->slots;
    if (within_budget)
    {
        budget->bytes -= records * SCHEMA_RECORD_BYTES;
        budget->slots -= slots;
    }
    else
        budget->bytes = 0;
    ctx.report = new_record(result);
    ctx.num_blockers = 0;
    ctx.work_exhausted = MY_FALSE;
    ctx.budget = budget;
    ctx.prepared = prepared;
    ctx.preparing_blueprint = preparing_blueprint;
    put_number(field(ctx.report, "old_generation"), old->schema_generation);
    put_number(field(ctx.report, "candidate_generation"), candidate->schema_generation);
    blocks = field(ctx.report, "blockers");
    put_array(blocks, allocate_array_unlimited(SCHEMA_MAX_BLOCKERS));
    ctx.blockers = blocks->u.vec;
    if (!within_budget)
        blocker(&ctx, "SCHEMA_MEMORY_LIMIT", NULL);
    if (within_budget && graph_compatible(&ctx, old, candidate))
    {
        defaults_ready = compare_variables(&ctx, old, candidate);
        /* The source pass prepares only required additions. Compatibility
         * blockers remain diagnostic in each target generation until the
         * complete validation/report phase; they must not erase staging
         * evidence by turning into a defaults-preparation exception.
         */
        if (preparing_blueprint)
            goto finish;
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
                    if (ctx.work_exhausted)
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
    return preparing_blueprint ? defaults_ready : ctx.num_blockers == 0;
} /* program_schema_compare() */

#if defined(DEBUG) && defined(BLUEPRINT_UPDATE_TESTING)
#include "object.h"
#include "../test/t-blueprint-update/defaults_native.inc"
#endif

#ifdef DEBUG
void
program_schema_check (const program_t *prog)

/* Schema tables own no separate references: existing types/name walkers
 * own every referenced value. Validate embedded offsets before ref checks.
 */
{
    assert(prog->schema_generation > 0);
    const struct schema_defaults_s *header = default_header(prog);
    assert(header);
    for (uint32_t i = 0; i < header->nodes; i++)
        assert(default_node_valid(prog, header, i));
    for (uint32_t i = 0; i < header->records; i++)
        assert(default_record_valid(header,
            (const struct schema_default_s *)(prog->schema_defaults + header->record_offset) + i));
    for (unsigned int i = 0; i < prog->num_variables; i++)
        assert(prog->variables[i].schema_default <= header->records);
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
