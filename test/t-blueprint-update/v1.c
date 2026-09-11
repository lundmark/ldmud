#pragma strong_types, save_types, init_variables

int hp = 100;
int obsolete = 9;

void set_hp(int value) { hp = value; }
int hp_value() { return hp; }
int charges_value() { return -1; }
int behavior_version() { return 1; }

#ifdef __BLUEPRINT_UPDATE__
int request_update(object source) { return update_blueprint(source); }
int request_explicit(object source, object *targets) { return update_blueprint(source, targets); }
mapping read_result(int id) { return update_blueprint_result(id); }
#endif
