#pragma save_types, lightweight, clone
int read_value() { return 19; }
closure handle() { return #'read_value; }
closure foreign(object ob) { return symbol_function("read_value", ob); }
