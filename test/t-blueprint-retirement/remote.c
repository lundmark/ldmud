#pragma lightweight
mixed kept;
void keep(mixed value) { kept = value; }
async void suspended(mixed value) { yield(); }
closure captured(mixed value) { return function mixed() { return value; }; }
