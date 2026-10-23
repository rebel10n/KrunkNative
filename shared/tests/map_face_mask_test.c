// map_face_mask_test: the CUBE face switches read from a raw object's "f" field, as v10's map loader
// reads them (deob-10.0.3.js: the loader's addBlock call takes f[0..5] from the list or from
// UTILS.convertFaces(hex string)). Bit i of the mask is Krunker face i (2 = top, 3 = bottom).
//
// Run from the repository root: ./<build>/map_face_mask_test
#include <shared.h>
#include <cJSON.h>

#include <stdio.h>

static int g_checks, g_failures;
#define CHECK(cond, ...) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int mask_of(const char *json) {
    cJSON *v = cJSON_Parse(json);
    const int mask = map_face_mask(v);
    cJSON_Delete(v);
    return mask;
}

int main(void) {
    // hex strings: digit i (MSB first) is face i, so "8" is top only and "38" is faces 0, 1, 2
    CHECK(mask_of("\"8\"") == 0x04, "\"8\" top only: got %x", mask_of("\"8\""));
    CHECK(mask_of("\"c\"") == 0x0c, "\"c\" top+bottom: got %x", mask_of("\"c\""));
    CHECK(mask_of("\"38\"") == 0x07, "\"38\" faces 0,1,2: got %x", mask_of("\"38\""));
    CHECK(mask_of("\"31\"") == 0x23, "\"31\" faces 0,1,5 (Vivo object 25): got %x", mask_of("\"31\""));
    CHECK(mask_of("\"2f\"") == 0x3d, "\"2f\" all but top: got %x", mask_of("\"2f\""));
    CHECK(mask_of("\"3f\"") == 0x3f, "\"3f\" all six faces");
    CHECK(mask_of("\"0\"") == 0x00, "\"0\" no faces");

    // lists: element i is face i's switch; 0 / false / "" off, 1 / true / missing / null on
    CHECK(mask_of("[1,1,1,1,1,0]") == 0x1f, "list with face 5 off (152_lostworld object 0): got %x", mask_of("[1,1,1,1,1,0]"));
    CHECK(mask_of("[0,0,1,0,0,0]") == 0x04, "list with top only: got %x", mask_of("[0,0,1,0,0,0]"));
    CHECK(mask_of("[0,0,0,1,0,0]") == 0x08, "list with bottom only: got %x", mask_of("[0,0,0,1,0,0]"));
    CHECK(mask_of("[1,1]") == 0x3f, "short list: missing elements are on: got %x", mask_of("[1,1]"));
    CHECK(mask_of("[1,null,false,\"\",\"0\",true]") == 0x33, "list falsy values (faces 0,1,4,5 on): got %x", mask_of("[1,null,false,\"\",\"0\",true]"));

    // absent or other values: every face (generateCube's default)
    CHECK(mask_of("null") == 0x3f, "null: all faces");
    CHECK(mask_of("5") == 0x3f, "number: all faces");
    CHECK(mask_of("\"\"") == 0x3f, "empty string: all faces");
    CHECK(map_face_mask(NULL) == 0x3f, "absent field: all faces");

    printf("map_face_mask_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
