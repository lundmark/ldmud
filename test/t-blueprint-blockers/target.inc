int value = 41;
mixed stored;
int read_value() { return value; }
closure foreign(object ob) { return symbol_function("read_value", ob); }
void keep(mixed arg) { stored = arg; }
mixed held() { return stored; }

closure handle(int kind)
{
    int captured = 7;
    switch (kind)
    {
        case 0: return #'read_value;
        case 1: return symbol_function("read_value", this_object());
        case 2: return function int() { return value; };
        case 3: return function int() { return value + captured; };
        case 4: return lambda(0, ({#'read_value}));
        case 5: return bind_lambda(unbound_lambda(0, 17));
        case 6: return symbol_variable("value");
        case 7: return symbol_variable(0);
        case 8: return restore_value(save_value(#'read_value));
        /* Synthetic inline names are hidden from restore lookup. A named
         * closure with saved context exercises the actual #c restore path.
         */
        case 9: return restore_value("#3:2\n#c:read_value:1:({7,})\n");
        case 10: return restore_value(save_value(symbol_variable("value")));
        case 11: return compile_string(0, "value", (<compile_string_options> use_object_variables: 1));
        case 12: return funcall(compile_string(({'a}), "function int() { return a; }"), 19);
        case 13: return #'sizeof;
        case 14: return unbound_lambda(0, 19);
    }
    return 0;
}

async int sleeper(int fail)
{
    yield(1);
    if (fail) raise_error("expected coroutine abort\n");
    return value;
}

async int waiter(coroutine other)
{
    return await(other);
}

async int wait_for(object next, int fail)
{
    return await(next.sleeper(fail));
}

async int chain(object next, object leaf, int fail)
{
    return await(next.wait_for(leaf, fail));
}

async int cyclic_frame(mixed *cycle)
{
    yield();
    return sizeof(cycle);
}

mixed *cycle(int kind)
{
    mixed *result = ({0});
    switch (kind)
    {
        case 0: result[0] = function int() { return sizeof(result) + value; }; break;
        case 1: result[0] = lambda(0, ({#'sizeof, quote(result)})); break;
        case 2: result[0] = bind_lambda(unbound_lambda(0, ({#'sizeof, quote(result)}))); break;
        case 3: result[0] = cyclic_frame(result); break;
        case 4:
            result[0] = restore_value("#3:2\n<1>=#c:read_value:1:({<1>,})\n");
            break;
    }
    return result;
}
