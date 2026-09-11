/* Compile the actual reusable implementation with small private test bounds.
 * coordinator_source.c is a symlink to the shipped mudlib source. */
#define BLUEPRINT_UPDATE_CAPACITY 2
#define BLUEPRINT_UPDATE_POLL_LIMIT 1
#define BLUEPRINT_UPDATE_INTERVAL 1
#define BLUEPRINT_UPDATE_CLOCK (find_object("coordinator").clock())
#include "/coordinator_source.c"
