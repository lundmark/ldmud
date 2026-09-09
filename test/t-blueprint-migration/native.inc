/* Private preparation assertions: no LPC inspection API or publication. */
static int migration_test_countdown = -1;

static Bool
migration_test_collect (program_update_request_t *request)
{
#ifdef GC_SUPPORT
    FILE *skip = fopen("migration-no-gc", "r");
    if (skip) { fclose(skip); return MY_FALSE; }
    size_t saved_array = max_array_size, saved_mapping = max_mapping_size, saved_keys = max_mapping_keys;
    int32 saved_eval = max_eval_cost, saved_file = max_file_xfer, saved_byte = max_byte_xfer;
    int32 saved_callouts = max_callouts, saved_use = use_eval_cost;
    int32 saved_cost = eval_cost, saved_assigned = assigned_eval_cost;
    p_int saved_memory = max_memory;
    int saved_privilege = malloc_privilege;
    /* Collection requires an empty VM, but must not execute the staging
     * rollback handler. This checkpoint is only called between complete
     * slot operations, with no materializer/key scratch left on the stack.
     */
    assert(inter_sp->type == T_ERROR_HANDLER && inter_sp->u.error_handler == &request->handler);
    inter_sp->type = T_INVALID;
    pop_stack();
    mark_end_evaluation();
    clear_state();
    garbage_collection();
    max_array_size = saved_array; max_mapping_size = saved_mapping; max_mapping_keys = saved_keys;
    max_eval_cost = saved_eval; max_file_xfer = saved_file; max_byte_xfer = saved_byte;
    max_callouts = saved_callouts; use_eval_cost = saved_use;
    eval_cost = saved_cost; assigned_eval_cost = saved_assigned;
    max_memory = saved_memory; malloc_privilege = saved_privilege;
    mark_start_evaluation();
    set_current_object(request->owner);
    push_error_handler(variables_cleanup, &request->handler);
    return MY_TRUE;
#else
    (void)request;
    return MY_FALSE;
#endif
}

static void
migration_test_begin (void)
{
    FILE *input;
    migration_test_countdown = -1;
    if (strcmp(get_txt(active->roots[UPDATE_ORIGIN].u.str), "/fault_target")) return;
    input = fopen("migration-fault", "r");
    if (input)
    {
        if (fscanf(input, "%d", &migration_test_countdown) != 1) migration_test_countdown = -1;
        fclose(input);
    }
}

void
program_update_migration_test_step (void)
{
    if (!active || migration_test_countdown < 0) return;
    if (migration_test_countdown--) return;
    migration_test_countdown = -1;
    /* Native collection with partial blocks rooted, after a completed
     * ownership operation. Never enter GC from inside free_svalue's queue.
     */
    Bool collected = migration_test_collect(active);
    debug_message("BLUEPRINT_MIGRATION_ROLLBACK_GC: partial preparation remains rooted, %d collections.\n", collected);
    errorf("update_blueprint(): injected partial variable preparation failure.\n");
}

static void
migration_test_checkpoint (program_update_request_t *request)
{
    program_t *candidate = request->source_from_path ? request->candidate : request->source_program;
    program_update_variables_t *blueprint = request->variables;
    FILE *input, *output;
    int expected, collect, objects = 0, checks = 0, collections = 0;
    Bool collected = MY_FALSE;
    const char *origin = get_txt(request->roots[UPDATE_ORIGIN].u.str);
#define MIGRATION_REQUIRE(condition) do { \
    checks++; \
    if (!(condition)) errorf("Blueprint migration native check %d failed at line %d.\n", checks, __LINE__); \
} while (0)

    if (strcmp(origin, "/migration_target") && strcmp(origin, "/aliases_target")
     && strcmp(origin, "/generations_target"))
        return;
    input = fopen("migration-expect", "r");
    if (!input) return;
    if (fscanf(input, "%d %d", &expected, &collect) != 2)
    {
        fclose(input);
        errorf("Invalid migration expectations.\n");
    }
    fclose(input);
    put_array(&request->roots[UPDATE_TEST_VALUES], allocate_array(16));
validate:
    objects = 0;
    while (blueprint->next) blueprint = blueprint->next;
    for (program_update_variables_t *stage = request->variables; stage; stage = stage->next)
    {
        object_t *object = stage->object.u.ob;
        mapping_t *schema;
        vector_t *matched, *added;
        MIGRATION_REQUIRE(object->prog == stage->old_program);
        MIGRATION_REQUIRE(object->variables != stage->values || !stage->num_values);
        if (stage->snapshot) continue;
        objects++;
        schema = variable_schema(request, stage->old_program);
        matched = report_field(schema, "matched")->u.vec;
        added = report_field(schema, "added")->u.vec;
        for (size_t i = 0; i < VEC_SIZE(matched); i++)
        {
            mapping_t *row = matched->item[i].u.map;
            int before = report_field(row, "old_slot")->u.number;
            int after = report_field(row, "new_slot")->u.number;
            MIGRATION_REQUIRE(same_variable_value(stage->values + after, object->variables + before));
            if (!collected && !strcmp(get_txt(stage->old_program->variables[before].name), "finished"))
            {
                svalue_t *scratch = &request->roots[UPDATE_TEST_VALUES].u.vec->item[0];
                schema_budget_t budget = { BLUEPRINT_UPDATE_MAX_BYTES, SCHEMA_MAX_WORK,
                                           BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS };
                MIGRATION_REQUIRE(stage->values[after].type == T_COROUTINE);
                assign_update_rvalue_no_free(scratch, object->variables + before, &budget);
                MIGRATION_REQUIRE(scratch->type == T_NUMBER && scratch->u.number == 0);
                MIGRATION_REQUIRE(object->variables[before].type == T_COROUTINE);
                free_svalue(scratch); put_number(scratch, 0);
            }
            if (!collected && !strcmp(get_txt(stage->old_program->variables[before].name), "dead"))
            {
                svalue_t *scratch = &request->roots[UPDATE_TEST_VALUES].u.vec->item[0];
                schema_budget_t budget = { BLUEPRINT_UPDATE_MAX_BYTES, SCHEMA_MAX_WORK,
                                           BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS };
                MIGRATION_REQUIRE(stage->values[after].type == T_OBJECT);
                assign_update_rvalue_no_free(scratch, object->variables + before, &budget);
                MIGRATION_REQUIRE(scratch->type == T_NUMBER && scratch->u.number == 0);
                MIGRATION_REQUIRE(object->variables[before].type == T_OBJECT);
                free_svalue(scratch); put_number(scratch, 0);
            }
            if (!strcmp(get_txt(stage->old_program->variables[before].name), "stored")
             && stage->values[after].type == T_POINTER)
            {
                vector_t *aliases = stage->values[after].u.vec;
                vector_t *scratch = request->roots[UPDATE_TEST_VALUES].u.vec;
                schema_budget_t budget = { BLUEPRINT_UPDATE_MAX_BYTES, SCHEMA_MAX_WORK,
                                           BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS };
                MIGRATION_REQUIRE(VEC_SIZE(aliases) == 11);
                for (int item = 0; item < 11; item++)
                {
                    svalue_t saved = aliases->item[item];
                    assign_update_rvalue_no_free(&scratch->item[item], &aliases->item[item], &budget);
                    MIGRATION_REQUIRE(same_variable_value(&saved, &aliases->item[item]));
                    MIGRATION_REQUIRE(scratch->item[item].type != T_LVALUE);
                }
                MIGRATION_REQUIRE(scratch->item[0].u.number == 41 || scratch->item[0].u.number == 83);
                MIGRATION_REQUIRE(scratch->item[1].type == T_STRING
                                  && !strcmp(get_txt(scratch->item[1].u.str), "bcd"));
                MIGRATION_REQUIRE(scratch->item[2].type == T_POINTER
                                  && VEC_SIZE(scratch->item[2].u.vec) == 2
                                  && scratch->item[2].u.vec->item[1].u.number == 2);
                MIGRATION_REQUIRE(scratch->item[3].u.number == 1);
                MIGRATION_REQUIRE(scratch->item[4].type == T_POINTER
                                  && scratch->item[4].u.vec->item[1].u.number == 3);
                MIGRATION_REQUIRE(scratch->item[5].u.number == 17);
                MIGRATION_REQUIRE(scratch->item[6].u.number == 'e');
                MIGRATION_REQUIRE(scratch->item[7].u.number == 31);
                MIGRATION_REQUIRE(scratch->item[8].type == T_NUMBER && !scratch->item[8].u.number);
                MIGRATION_REQUIRE(scratch->item[9].type == T_POINTER
                                  && !scratch->item[9].u.vec->item[0].u.number
                                  && !scratch->item[9].u.vec->item[1].u.number);
                MIGRATION_REQUIRE(scratch->item[10].type == T_POINTER
                                  && scratch->item[10].u.vec->item[0].u.number == scratch->item[0].u.number
                                  && scratch->item[10].u.vec->item[1].u.number == 17);
                for (int item = 0; item < 11; item++)
                {
                    free_svalue(&scratch->item[item]); put_number(&scratch->item[item], 0);
                }
                /* A one-slot range consumes its actual bytes and preserves
                 * the one-byte request stop sentinel at the exact boundary.
                 */
                budget = (schema_budget_t){ sizeof(vector_t) + 2 * sizeof(svalue_t) + 1, 3, 2 };
                assign_update_rvalue_no_free(&scratch->item[0], &aliases->item[2], &budget);
                MIGRATION_REQUIRE(budget.bytes == 1 && budget.work == 0 && budget.slots == 0);
                free_svalue(&scratch->item[0]); put_number(&scratch->item[0], 0);
            }
            if (!collected && !strcmp(get_txt(stage->old_program->variables[before].name), "stale"))
            {
                svalue_t *scratch = &request->roots[UPDATE_TEST_VALUES].u.vec->item[0];
                schema_budget_t budget = { BLUEPRINT_UPDATE_MAX_BYTES, SCHEMA_MAX_WORK,
                                           BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS };
                MIGRATION_REQUIRE(stage->values[after].type == T_CLOSURE);
                assign_update_rvalue_no_free(scratch, object->variables + before, &budget);
                MIGRATION_REQUIRE(same_variable_value(scratch, object->variables + before));
                MIGRATION_REQUIRE(object->variables[before].type == T_CLOSURE);
                free_svalue(scratch); put_number(scratch, 0);
            }
            if (!collected && !strcmp(get_txt(stage->old_program->variables[before].name), "stale_cell"))
            {
                svalue_t *scratch = &request->roots[UPDATE_TEST_VALUES].u.vec->item[0];
                schema_budget_t budget = { BLUEPRINT_UPDATE_MAX_BYTES, SCHEMA_MAX_WORK,
                                           BLUEPRINT_UPDATE_MAX_VARIABLE_SLOTS };
                MIGRATION_REQUIRE(stage->values[after].type == T_LVALUE);
                MIGRATION_REQUIRE(stage->values[after].u.protected_lvalue->val.type == T_CLOSURE);
                assign_update_rvalue_no_free(scratch, object->variables + before, &budget);
                MIGRATION_REQUIRE(scratch->type == T_NUMBER && !scratch->u.number);
                MIGRATION_REQUIRE(stage->values[after].u.protected_lvalue->val.type == T_CLOSURE);
                free_svalue(scratch); put_number(scratch, 0);
            }
        }
        for (size_t i = 0; i < VEC_SIZE(added); i++)
        {
            mapping_t *row = added->item[i].u.map;
            int slot = report_field(row, "new_slot")->u.number;
            svalue_t *value = stage->values + slot;
            Bool shared = stage != blueprint && !(candidate->variables[slot].type.t_flags & VAR_INITIALIZED);
            MIGRATION_REQUIRE(value->type != T_INVALID);
            if (shared)
            {
                svalue_t *source = blueprint->values + slot;
                if (source->type == T_LVALUE && source->x.lvalue_type == LVALUE_PROTECTED)
                    source = &source->u.protected_lvalue->val;
                MIGRATION_REQUIRE(value->type != T_LVALUE);
                MIGRATION_REQUIRE(same_variable_value(value, source));
                if (blueprint->values[slot].type == T_LVALUE)
                {
                    /* Reseating a NEW clone slot must not assign through the
                     * externally protected source cell. Mutable contents are
                     * still shared by the pointer assertion above.
                     */
                    svalue_t *scratch = &request->roots[UPDATE_TEST_VALUES].u.vec->item[0];
                    assign_update_svalue_no_free(scratch, value);
                    assign_svalue(value, &const0);
                    MIGRATION_REQUIRE(source->type == T_POINTER && source->u.vec == scratch->u.vec);
                    free_svalue(value);
                    assign_update_svalue_no_free(value, scratch);
                    free_svalue(scratch); put_number(scratch, 0);
                }
            }
            else if (value->type == T_POINTER)
            {
                svalue_t *description = report_field(row, "default");
                svalue_t *diagnostic = report_field(description->u.map, "value");
                if (diagnostic->type == T_POINTER && VEC_SIZE(value->u.vec))
                    MIGRATION_REQUIRE(value->u.vec != diagnostic->u.vec);
                for (program_update_variables_t *other = stage->next; other; other = other->next)
                    if (!other->snapshot && VEC_SIZE(value->u.vec))
                        MIGRATION_REQUIRE(value->u.vec != other->values[slot].u.vec);
                if (!strcmp(get_txt(candidate->variables[slot].name), "fresh"))
                {
                    vector_t *array = value->u.vec;
                    MIGRATION_REQUIRE(VEC_SIZE(array) == 2 && array->item[0].type == T_POINTER);
                    MIGRATION_REQUIRE(array->item[0].u.vec->item[0].u.number == 19);
                    MIGRATION_REQUIRE(array->item[1].type == T_MAPPING);
                    if (diagnostic->type == T_POINTER)
                    {
                        MIGRATION_REQUIRE(array->item[0].u.vec != diagnostic->u.vec->item[0].u.vec);
                        MIGRATION_REQUIRE(array->item[1].u.map != diagnostic->u.vec->item[1].u.map);
                    }
                    if (stage == blueprint && request->source_from_path)
                    {
                        /* The source prepass owns a different diagnostic
                         * aggregate from the clone-policy schema row.
                         */
                        svalue_t *source_default = &request->roots[UPDATE_BLUEPRINT_DEFAULTS].u.vec->item[slot];
                        MIGRATION_REQUIRE(source_default->type == T_POINTER);
                        MIGRATION_REQUIRE(array != source_default->u.vec);
                        MIGRATION_REQUIRE(array->item[0].u.vec != source_default->u.vec->item[0].u.vec);
                        MIGRATION_REQUIRE(array->item[1].u.map != source_default->u.vec->item[1].u.map);
                    }
                }
            }
        }
    }
    MIGRATION_REQUIRE(objects == expected);
    if (collect && !collected)
    {
        collections += migration_test_collect(request);
        collected = MY_TRUE;
        goto validate;
    }
    output = fopen("migration-observed", "w");
    if (!output) errorf("Cannot write migration checkpoint.\n");
    fprintf(output, "%d\n", objects);
    fclose(output);
    debug_message("BLUEPRINT_MIGRATION_NATIVE: %d checks, %d objects, %d collections.\n", checks, objects, collections);
#undef MIGRATION_REQUIRE
}
