#include "/inc/deep_eq.inc"

string kind, mode;
object owner;
mixed first, second, third, last;
mixed original, copied, replacement;

void require(int condition, string description)
{
    if (!condition)
        raise_error(kind + "/" + mode + ": " + description + "\n");
}

mixed initial_value()
{
    switch (kind)
    {
        case "bytes-range":
        case "bytes-char": return b"abcd";
        case "array-range": return ({ 1, 2, 3, 4 });
        case "unicode-char": return "a\u00e4cd";
        default: return "abcd";
    }
}

int is_range() { return kind[<5..] == "range"; }

void setup(string new_kind, string new_mode)
{
    kind = new_kind;
    mode = new_mode;
    original = initial_value();
    if (mode == "temporary")
    {
        /* Discard the temporary's variable. Range normalization will
         * detach it; two aliases prevent collapse before collection.
         */
        mixed temporary = initial_value();
        if (is_range()) first = &(temporary[1..2]);
        else first = &(initial_value()[1]);
        temporary = 0;
    }
    else
    {
        owner = clone_object("owner");
        owner->set_value(initial_value());
        mixed whole = &(owner->value_ref());
        if (is_range()) first = &(whole[1..2]);
        else
        {
            first = &(whole[1]);
            /* A distinct character lvalue retains the same backing cell. */
            last = &(whole[3]);
        }
        copied = owner->query_value();
        if (mode == "detached")
        {
            replacement = kind == "array-range" ? ({ 9 })
                        : kind[0..4] == "bytes" ? b"replacement"
                        : "replacement";
            owner->set_value(replacement);
        }
        else if (mode == "retyped")
        {
            /* Character lvalues retain their old backing cell even when
             * it now contains another type. DEBUG walkers must visit it.
             */
            replacement = ({ clone_object("owner") });
            owner->set_value(replacement);
            destruct(owner);
        }
        else if (mode == "destroyed")
            destruct(owner);
    }
    second = &first;
    third = &first;
}

void check(int stage)
{
    mixed part, whole;
    if (is_range())
    {
        if (kind == "array-range")
        {
            part = stage == 1 ? ({ 7 }) : stage == 2 ? ({ 8, 9, 10 }) : ({ 5 });
            whole = ({ 1 }) + part + ({ 4 });
        }
        else if (kind == "bytes-range")
        {
            part = stage == 1 ? b"Z" : stage == 2 ? b"XYZ" : b"Q";
            whole = b"a" + part + b"d";
        }
        else
        {
            part = stage == 1 ? "Z" : stage == 2 ? "XYZ" : "Q";
            whole = "a" + part + "d";
        }
    }
    else
    {
        /* Exercise ASCII writes and both growth and shrinkage of UTF-8. */
        part = kind == "unicode-char"
             ? (stage == 1 ? 0x1f600 : stage == 2 ? 'Z' : 0x20ac)
             : (stage == 1 ? 'Z' : stage == 2 ? 'Q' : 'R');
        whole = kind == "bytes-char" ? b"a" + to_bytes(({ part })) + b"cd"
              : "a" + sprintf("%c", part) + "cd";
    }

    if (stage == 1)
    {
        first = part;
        require(deep_eq(first, part), "first alias write");
    }
    else second = part;
    require(deep_eq(second, part), "surviving alias write");
    if (stage < 3)
        require(deep_eq(third, part), "other surviving alias observes write");
    else
        require(third == 0, "second released slot remains empty");
    if (stage > 1)
        require(first == 0, "released slot remains empty");
    if (owner)
        require(deep_eq(owner->query_value(), mode == "attached" ? whole : replacement),
                "backing variable after alias write");
    if (mode != "temporary")
    {
        require(deep_eq(copied, original), "ordinary copy remains independent");
        if (!is_range()) require(last == 'd', "distinct character alias position");
    }

    if (stage == 1)
    {
        /* Reseating the variable requires the ampersand on the left. */
        &first = 0;
        require(first == 0 && deep_eq(second, part) && deep_eq(third, part),
                "release one alias while two survive");
    }
    else if (stage == 2)
    {
        &third = 0;
        require(third == 0 && deep_eq(second, part),
                "release another alias while one survives");
        if (owner) destruct(owner);
    }
}

void release()
{
    if (mode == "retyped") destruct(replacement[0]);
    destruct(this_object());
}
