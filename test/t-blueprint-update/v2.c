#pragma strong_types, save_types, init_variables

int charges = 5;
int hp = 100;

void set_hp(int value) { hp = value; }
int hp_value() { return hp; }
int charges_value() { return charges; }
int behavior_version() { return 2; }

#ifdef __BLUEPRINT_UPDATE__
int request_update(object source) { return update_blueprint(source); }
int request_explicit(object source, object *targets) { return update_blueprint(source, targets); }
mapping read_result(int id) { return update_blueprint_result(id); }
#endif
