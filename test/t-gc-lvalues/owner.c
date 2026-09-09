mixed value;

void set_value(mixed new_value) { value = new_value; }
mixed value_ref() { return &value; }
mixed query_value() { return value; }
