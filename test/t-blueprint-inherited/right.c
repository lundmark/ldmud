#pragma strong_types, save_types, init_variables
virtual inherit "base";
private int duplicate=23;
int result() { return duplicate; }
closure cell() { return #'duplicate; }
