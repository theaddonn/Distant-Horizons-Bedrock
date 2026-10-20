#include <doctest.h>

#include "dh/config/KeyNames.h"

using namespace dh::config;

TEST_CASE("key names: none and empty") {
    CHECK(parseKeyName("none") == 0);
    CHECK(parseKeyName("") == 0);
    CHECK(parseKeyName("NONE") == 0);
    CHECK(isValidKeyName("none"));
    CHECK(isValidKeyName(""));
}

TEST_CASE("key names: function keys") {
    CHECK(parseKeyName("f1") == 0x70);
    CHECK(parseKeyName("F5") == 0x74);
    CHECK(parseKeyName("f12") == 0x7B);
    CHECK(parseKeyName("f24") == 0x87);
    CHECK(parseKeyName("f0") == 0);
    CHECK(parseKeyName("f25") == 0);
    CHECK(parseKeyName("f01") == 0);
    CHECK_FALSE(isValidKeyName("f25"));
}

TEST_CASE("key names: numpad") {
    CHECK(parseKeyName("numpad0") == 0x60);
    CHECK(parseKeyName("Numpad9") == 0x69);
    CHECK(parseKeyName("numpad-decimal") == 0x6E);
    CHECK(parseKeyName("numpad-divide") == 0x6F);
    CHECK(parseKeyName("numpad-multiply") == 0x6A);
    CHECK(parseKeyName("numpad-subtract") == 0x6D);
    CHECK(parseKeyName("numpad-add") == 0x6B);
    CHECK(parseKeyName("numpad10") == 0);
}

TEST_CASE("key names: letters, digits, navigation") {
    CHECK(parseKeyName("a") == 'A');
    CHECK(parseKeyName("Z") == 'Z');
    CHECK(parseKeyName("0") == '0');
    CHECK(parseKeyName("9") == '9');
    CHECK(parseKeyName("insert") == 0x2D);
    CHECK(parseKeyName("delete") == 0x2E);
    CHECK(parseKeyName("home") == 0x24);
    CHECK(parseKeyName("end") == 0x23);
    CHECK(parseKeyName("pageup") == 0x21);
    CHECK(parseKeyName("pagedown") == 0x22);
    CHECK(parseKeyName("pause") == 0x13);
    CHECK(parseKeyName("scrolllock") == 0x91);
}

TEST_CASE("key names: invalid") {
    CHECK(parseKeyName("banana") == 0);
    CHECK(parseKeyName("ctrl") == 0);
    CHECK(parseKeyName("ab") == 0);
    CHECK_FALSE(isValidKeyName("banana"));
}

TEST_CASE("key names: vanilla collisions") {
    for (const char* n : {"f1", "f2", "f3", "f5", "f11"}) CHECK(isVanillaKey(parseKeyName(n)));
    for (const char* n : {"f4", "f6", "f12", "numpad0", "a", "none"}) CHECK_FALSE(isVanillaKey(parseKeyName(n)));
    CHECK_FALSE(isVanillaKey(0));
}
