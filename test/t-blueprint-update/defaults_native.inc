/* Test-only native boundary checks, included beside the private reader. */
struct default_test_limits_s
{
    error_handler_t handler;
    size_t array, mapping, keys;
};

static void
default_test_restore_limits (error_handler_t *handler)
{
    struct default_test_limits_s *limits = (struct default_test_limits_s *)handler;
    max_array_size = limits->array;
    max_mapping_size = limits->mapping;
    max_mapping_keys = limits->keys;
    xfree(limits);
}

void
program_schema_test_defaults (program_t *source, svalue_t *root)
{
    struct default_test_program_s
    {
        program_t program;
        struct schema_defaults_s header;
        union { void *alignment; struct schema_default_s values[2]; } records;
        struct schema_default_node_s nodes[BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH + 1];
        uint32_t edges[BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH + 1];
        union { void *alignment; char text[8]; } bytes;
        variable_t variables[3];
    } fixture = {0}, saved;
    schema_budget_t budget;
    schema_compare_t ctx = {0};
    svalue_t *value;
    int checks = 0;
    size_t array_bytes = sizeof(vector_t) + sizeof(svalue_t);
#define NATIVE_REQUIRE(condition) do { \
    checks++; \
    if (!(condition)) \
        errorf("Blueprint native default check %d failed at line %d.\n", checks, __LINE__); \
} while (0)
#define RESET_VALUE() do { free_svalue(value); put_number(value, 0); } while (0)
#define RESET_BUDGET() do { budget = (schema_budget_t) { \
    BLUEPRINT_UPDATE_MAX_BYTES, SCHEMA_MAX_WORK, BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS }; } while (0)

    fixture.program.total_size = sizeof(fixture);
    fixture.program.schema_defaults = (bytecode_p)&fixture.header;
    fixture.program.schema_defaults_size = (char *)&fixture.variables[0] - (char *)&fixture.header;
    fixture.program.schema_generation = 1;
    fixture.program.name = source->name;
    fixture.program.variables = &fixture.variables[0];
    fixture.program.num_variables = 1;
    fixture.variables[0].name = source->variables[0].name;
    fixture.variables[0].schema_declared = true;
    fixture.variables[0].schema_default = 1;
    fixture.variables[0].type.t_type = lpctype_mixed;
    fixture.variables[0].type.t_flags = VAR_INITIALIZED;
    fixture.header.version = SCHEMA_DEFAULT_VERSION;
    fixture.header.records = 1;
    fixture.header.record_offset = (char *)&fixture.records.values - (char *)&fixture.header;
    fixture.header.nodes = 2;
    fixture.header.node_offset = (char *)&fixture.nodes - (char *)&fixture.header;
    fixture.header.edges = 1;
    fixture.header.edge_offset = (char *)&fixture.edges - (char *)&fixture.header;
    fixture.header.bytes = sizeof(fixture.bytes);
    fixture.header.byte_offset = (char *)&fixture.bytes - (char *)&fixture.header;
    fixture.records.values[0].status = SCHEMA_DEFAULT_SUPPORTED;
    fixture.records.values[0].root = 1;
    fixture.nodes[0].kind = SCHEMA_DEFAULT_INTEGER;
    fixture.nodes[0].depth = 1;
    fixture.nodes[0].value.integer = 7;
    fixture.nodes[1].kind = SCHEMA_DEFAULT_ARRAY;
    fixture.nodes[1].depth = 2;
    fixture.nodes[1].edge_count = 1;
    saved = fixture;
    NATIVE_REQUIRE(default_header(&fixture.program));
    fixture.program.schema_defaults = (bytecode_p)&fixture.program;
    NATIVE_REQUIRE(!default_header(&fixture.program));
    fixture = saved;
    fixture.program.schema_defaults_size = sizeof(fixture);
    NATIVE_REQUIRE(!default_header(&fixture.program));
    fixture = saved;
    fixture.header.record_offset = 0;
    NATIVE_REQUIRE(!default_header(&fixture.program));
    fixture = saved;
    fixture.header.node_offset = fixture.header.record_offset;
    NATIVE_REQUIRE(!default_header(&fixture.program));
    fixture = saved;
    fixture.header.edges = UINT32_MAX;
    NATIVE_REQUIRE(!default_header(&fixture.program));
    fixture = saved;
    fixture.header.byte_offset++;
    NATIVE_REQUIRE(!default_header(&fixture.program));
    fixture = saved;
    NATIVE_REQUIRE(default_node_valid(&fixture.program, &fixture.header, 1));
    fixture.edges[0] = 1;
    NATIVE_REQUIRE(!default_node_valid(&fixture.program, &fixture.header, 1));
    fixture = saved;
    fixture.nodes[1].depth = 1;
    NATIVE_REQUIRE(!default_node_valid(&fixture.program, &fixture.header, 1));
    fixture = saved;
    fixture.nodes[0].edge_count = 1;
    NATIVE_REQUIRE(!default_node_valid(&fixture.program, &fixture.header, 0));
    fixture = saved;
    fixture.nodes[0].kind = SCHEMA_DEFAULT_FLOAT;
    fixture.nodes[0].value.floating = HUGE_VAL;
    NATIVE_REQUIRE(!default_node_valid(&fixture.program, &fixture.header, 0));
    fixture = saved;
    fixture.records.values[0].source_size = UINT32_MAX;
    NATIVE_REQUIRE(!default_record_valid(&fixture.header, &fixture.records.values[0]));
    fixture = saved;
    fixture.records.values[0].status = UINT32_MAX;
    NATIVE_REQUIRE(!default_record_valid(&fixture.header, &fixture.records.values[0]));
    fixture = saved;
    fixture.records.values[0].root = fixture.header.nodes;
    NATIVE_REQUIRE(!default_record_valid(&fixture.header, &fixture.records.values[0]));
    fixture = saved;

    put_array(root, allocate_array(6));
    ctx.report = new_record(&root->u.vec->item[0]);
    put_array(field(ctx.report, "blockers"), allocate_array_unlimited(SCHEMA_MAX_BLOCKERS));
    ctx.blockers = field(ctx.report, "blockers")->u.vec;
    ctx.budget = &budget;
    value = &root->u.vec->item[1];
    RESET_BUDGET();
    budget.work = 3;
    budget.slots = 2;
    budget.bytes = array_bytes + 1; /* Preserve the request stop sentinel. */
    NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
    NATIVE_REQUIRE(!budget.work && !budget.slots && budget.bytes == 1);
    NATIVE_REQUIRE(value->u.vec->item[0].u.number == 7);
    RESET_VALUE();
    RESET_BUDGET();
    budget.work = 2;
    NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
    RESET_VALUE();
    RESET_BUDGET();
    budget.slots = 1;
    NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
    RESET_VALUE();
    RESET_BUDGET();
    budget.bytes = array_bytes;
    NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
    RESET_VALUE();
    RESET_BUDGET();
    budget.slots = 3;
    NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
    RESET_VALUE();
    NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
    RESET_VALUE();
    RESET_BUDGET();
    NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
    NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1,
                                      &root->u.vec->item[2], 0));
    value->u.vec->item[0].u.number = 99;
    NATIVE_REQUIRE(root->u.vec->item[2].u.vec->item[0].u.number == 7);
    RESET_VALUE();
    {
        struct default_test_limits_s *limits = xalloc(sizeof(*limits));
        if (!limits) outofmemory("native default limit restoration");
        limits->array = max_array_size;
        limits->mapping = max_mapping_size;
        limits->keys = max_mapping_keys;
        push_error_handler(default_test_restore_limits, &limits->handler);
        max_array_size = 1;
        RESET_BUDGET();
        NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        RESET_VALUE();
        fixture.nodes[1].edge_count = fixture.header.edges = 2;
        RESET_BUDGET();
        NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        max_array_size = limits->array;
        fixture.nodes[1].kind = SCHEMA_DEFAULT_MAPPING;
        fixture.nodes[1].width = 1;
        max_mapping_keys = 1;
        max_mapping_size = 2;
        RESET_BUDGET();
        NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        RESET_VALUE();
        fixture.nodes[1].edge_count = fixture.header.edges = 4;
        max_mapping_keys = 2;
        max_mapping_size = 4;
        RESET_BUDGET();
        NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        NATIVE_REQUIRE(value->u.map->num_entries == 1); /* Duplicate input rows count. */
        RESET_VALUE();
        max_mapping_size = 3;
        RESET_BUDGET();
        NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        max_mapping_size = 4;
        max_mapping_keys = 1;
        RESET_BUDGET();
        NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        fixture.nodes[1].edge_count = fixture.header.edges = 2;
        fixture.nodes[1].width = 0;
        max_mapping_keys = 2;
        max_mapping_size = 2;
        RESET_BUDGET();
        NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        RESET_VALUE();
        max_mapping_size = 1;
        RESET_BUDGET();
        NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        fixture.nodes[1].edge_count = 0;
        fixture.nodes[1].depth = 1;
        fixture.nodes[1].width = PINT_MAX;
        RESET_BUDGET();
        NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header, 1, value, 0));
        pop_stack(); /* Restore limits on success, and on any thrown failure. */
        fixture = saved;
    }
    {
        struct default_test_program_s child = fixture;
        inherit_t inherit[2] = {{0}, {0}};
        child.program.schema_defaults = (bytecode_p)&child.header;
        child.program.variables = child.variables;
        child.program.num_variables = 3;
        child.program.num_inherited = 2;
        child.program.inherit = inherit;
        child.nodes[0].value.integer = 19;
        for (int i = 0; i < 3; i++)
        {
            child.variables[i] = fixture.variables[0];
            child.variables[i].schema_default = i == 2 ? 1 : 0;
            child.variables[i].schema_declared = i == 2;
            if (i < 2)
            {
                inherit[i].prog = &fixture.program;
                inherit[i].inherit_depth = 1;
                inherit[i].inherit_type = INHERIT_TYPE_NORMAL;
                inherit[i].variable_index_offset = i;
            }
            else
                child.variables[i].type.t_flags |= NAME_HIDDEN;
        }
        RESET_BUDGET();
        for (int i = 0; i < 3; i++)
        {
            schema_identity_t identity = { .path = "$" };
            NATIVE_REQUIRE(variable_identity(&ctx, &child.program, i, &identity, 0));
            NATIVE_REQUIRE(identity.program == (i == 2 ? &child.program : &fixture.program));
            NATIVE_REQUIRE(identity.slot == (i == 2 ? 2 : 0));
            NATIVE_REQUIRE(!strcmp(identity.path, i == 0 ? "$/n0" : i == 1 ? "$/n1" : "$"));
            declaration(value, &identity, -1, i);
            describe_default(&ctx, &child.program, i, &identity, value);
            NATIVE_REQUIRE(field(field(value->u.map, "default")->u.map, "value")
                           ->u.vec->item[0].u.number == (i == 2 ? 19 : 7));
            RESET_VALUE();
        }
    }
    for (unsigned int i = 1; i <= BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH; i++)
    {
        fixture.nodes[i] = fixture.nodes[1];
        fixture.nodes[i].depth = i + 1;
        fixture.nodes[i].edge_start = i - 1;
        fixture.edges[i - 1] = i - 1;
    }
    fixture.header.nodes = BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH + 1;
    fixture.header.edges = BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH;
    RESET_BUDGET();
    NATIVE_REQUIRE(materialize_default(&ctx, &fixture.program, &fixture.header,
                                      BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH - 1, value, 0));
    RESET_VALUE();
    NATIVE_REQUIRE(!materialize_default(&ctx, &fixture.program, &fixture.header,
                                       BLUEPRINT_UPDATE_MAX_LITERAL_DEPTH, value, 0));
    RESET_VALUE();
    {
        Bool exhausted;
        size_t cost;
        lpctype_t *type = get_array_type(lpctype_int);
        if (!type) outofmemory("native default int array type");
        put_lpctype(&root->u.vec->item[3], type);
        type = get_array_type(lpctype_string);
        if (!type) outofmemory("native default string array type");
        put_lpctype(&root->u.vec->item[4], type);
        type = get_union_type(root->u.vec->item[3].u.lpctype,
                              root->u.vec->item[4].u.lpctype);
        if (!type) outofmemory("native default union type");
        put_lpctype(&root->u.vec->item[5], type);
        put_array(value, allocate_array(2));
        put_number(&value->u.vec->item[0], 1);
        put_number(&value->u.vec->item[1], 2);
        RESET_BUDGET();
        NATIVE_REQUIRE(check_rtt_compatibility(type, value));
        NATIVE_REQUIRE(check_rtt_compatibility_bounded(type, value, &budget.work, &exhausted));
        NATIVE_REQUIRE(!exhausted);
        cost = SCHEMA_MAX_WORK - budget.work;
        budget.work = cost;
        NATIVE_REQUIRE(check_rtt_compatibility_bounded(type, value, &budget.work, &exhausted));
        NATIVE_REQUIRE(!exhausted && !budget.work);
        budget.work = cost - 1;
        NATIVE_REQUIRE(!check_rtt_compatibility_bounded(type, value, &budget.work, &exhausted));
        NATIVE_REQUIRE(exhausted);
        put_c_string(&value->u.vec->item[1], "x");
        RESET_BUDGET();
        NATIVE_REQUIRE(!check_rtt_compatibility(type, value));
        NATIVE_REQUIRE(!check_rtt_compatibility_bounded(type, value, &budget.work, &exhausted));
        NATIVE_REQUIRE(!exhausted);
        cost = SCHEMA_MAX_WORK - budget.work;
        budget.work = cost - 1;
        NATIVE_REQUIRE(!check_rtt_compatibility_bounded(type, value, &budget.work, &exhausted));
        NATIVE_REQUIRE(exhausted);
        RESET_VALUE();
    }
    free_svalue(root);
    put_number(root, 0);
    fixture = saved;
    {
        program_t old = fixture.program;
        /* Retained and shared-copy decisions ignore even malformed/missing
         * metadata. A newly independent declaration must demand it.
         */
        old.schema_generation = 2;
        fixture.program.schema_defaults = (bytecode_p)&fixture.program;
        RESET_BUDGET();
        NATIVE_REQUIRE(program_schema_compare(&old, &fixture.program, root,
                                               &budget, NULL, MY_FALSE));
        free_svalue(root);
        put_number(root, 0);
        {
            variable_t retained = fixture.variables[0];
            old.variables = &retained;
            retained.type.t_flags ^= TYPE_MOD_PRIVATE;
            RESET_BUDGET();
            NATIVE_REQUIRE(!program_schema_compare(&old, &fixture.program, root,
                                                    &budget, NULL, MY_FALSE));
            free_svalue(root);
            put_number(root, 0);
            RESET_BUDGET();
            NATIVE_REQUIRE(program_schema_compare(&old, &fixture.program, root,
                                                   &budget, NULL, MY_TRUE));
            free_svalue(root);
            put_number(root, 0);
            RESET_BUDGET();
            budget.work = 0;
            NATIVE_REQUIRE(!program_schema_compare(&old, &fixture.program, root,
                                                    &budget, NULL, MY_TRUE));
            free_svalue(root);
            put_number(root, 0);
            old.variables = fixture.variables;
        }
        old.num_variables = 0;
        fixture.variables[0].type.t_flags = 0;
        fixture.program.blueprint = source->blueprint;
        RESET_BUDGET();
        NATIVE_REQUIRE(program_schema_compare(&old, &fixture.program, root,
                                               &budget, NULL, MY_FALSE));
        free_svalue(root);
        put_number(root, 0);
        fixture.variables[0].type.t_flags = VAR_INITIALIZED;
        RESET_BUDGET();
        NATIVE_REQUIRE(!program_schema_compare(&old, &fixture.program, root,
                                                &budget, NULL, MY_FALSE));
        free_svalue(root);
        put_number(root, 0);
    }
    debug_message("BLUEPRINT_DEFAULT_NATIVE: %d checks passed.\n", checks);
#undef RESET_BUDGET
#undef RESET_VALUE
#undef NATIVE_REQUIRE
}

void
program_schema_test_rtt (program_t *prog)
{
    FILE *input;
    int check, warn;
    const struct schema_defaults_s *header;
    if (strcmp(get_txt(prog->name), "validation_target.c"))
        return;
    input = fopen("defaults-rtt", "r");
    if (!input)
        return;
    if (fscanf(input, "%d %d", &check, &warn) != 2)
    {
        fclose(input);
        errorf("Invalid native RTT fixture expectations.\n");
    }
    fclose(input);
    header = default_header(prog);
    if (!header)
        errorf("Native default RTT header missing.\n");
    for (int i = 0; i < prog->num_variables; i++)
        if (!strcmp(get_txt(prog->variables[i].name), "added"))
        {
            const struct schema_default_s *record =
                (const struct schema_default_s *)(prog->schema_defaults + header->record_offset)
                    + prog->variables[i].schema_default - 1;
            if (!!(record->flags & SCHEMA_DEFAULT_RTT_CHECK) != check
             || !!(prog->flags & P_WARN_RTT_CHECKS) != warn)
                errorf("Native default RTT pragma capture mismatch.\n");
            if (record->status == SCHEMA_DEFAULT_SUPPORTED && prog->blueprint)
            {
                const struct schema_default_node_s *node =
                    (const struct schema_default_node_s *)(prog->schema_defaults + header->node_offset)
                        + record->root;
                if (node->kind == SCHEMA_DEFAULT_FLOAT)
                {
                    double ordinary = READ_DOUBLE(&prog->blueprint->variables[i]);
                    if (ordinary != node->value.floating
                     || !!signbit(ordinary) != !!signbit(node->value.floating))
                        errorf("Native default float payload differs from ordinary initialization.\n");
                }
            }
            debug_message("BLUEPRINT_DEFAULT_RTT: initializer emission and final warning flags match.\n");
            return;
        }
    errorf("Native default RTT declaration missing.\n");
}
