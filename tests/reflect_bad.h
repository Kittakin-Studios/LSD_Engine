// Must be rejected by lsd_reflect_gen (CTest expects the generator to fail on this file).
#include "lsd/reflect/reflect.h"
struct Base {};
LSD_STRUCT(Componnent)
struct Bad : Base {
    LSD_GENERATED()
};
