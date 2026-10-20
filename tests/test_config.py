#!/usr/bin/env python3
"""Exercise real wheel-assignment and config codec code under sanitizers."""
from pathlib import Path
import itertools
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include "tpms_config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    TpmsConfig original;
    tpms_config_default(&original);
    for(unsigned i = 0; i < TPMS_WHEEL_COUNT; i++) assert(original.ids[i] == 0);
    /* Invented IDs keep test fixtures independent of any real vehicle. */
    original.ids[0] = 0x11223344;
    original.ids[1] = 0x22334455;
    original.ids[2] = 0x33445566;
    original.ids[3] = 0x44556677;
    assert(tpms_config_position(&original, 0) == -1);
    assert(tpms_config_position(&original, 0x12345678) == -1);
    for(unsigned i = 0; i < 4; i++) assert(tpms_config_position(&original, original.ids[i]) == (int)i);
    TpmsConfig c = original;
    assert(!tpms_config_assign(&c, 4, original.ids[0]));
    assert(!tpms_config_assign(&c, 0, original.ids[0]));
    assert(c.generation == original.generation);
    /* Rotation swaps keep every sensor assigned exactly once. */
    assert(tpms_config_assign(&c, 0, original.ids[1]));
    assert(c.ids[0] == original.ids[1] && c.ids[1] == original.ids[0]);
    assert(c.generation == original.generation + 1);
    assert(tpms_config_assign(&c, 0, 0));
    assert(c.ids[0] == 0 && c.ids[1] == original.ids[0]);
    assert(tpms_config_assign(&c, 0, original.ids[2]));
    assert(c.ids[2] == 0 && c.ids[0] == original.ids[2]);
    assert(tpms_config_assign(&c, 2, 0x12345678));
    assert(c.ids[2] == 0x12345678 && tpms_config_position(&c, original.ids[1]) == -1);
    uint8_t bytes[TPMS_CONFIG_BYTES], broken[TPMS_CONFIG_BYTES];
    tpms_config_encode(&c, bytes);
    TpmsConfig loaded = {0};
    assert(tpms_config_decode(&loaded, bytes, sizeof(bytes)));
    assert(!memcmp(&c, &loaded, sizeof(c)));
    for(size_t i = 0; i < sizeof(bytes); i++) {
        memcpy(broken, bytes, sizeof(bytes)); broken[i] ^= 1;
        TpmsConfig untouched = loaded;
        assert(!tpms_config_decode(&loaded, broken, sizeof(broken)));
        assert(!memcmp(&loaded, &untouched, sizeof(loaded)));
        assert(!tpms_config_decode(&loaded, bytes, i));
    }
    assert(!tpms_config_decode(&loaded, bytes, sizeof(bytes) + 1));
    c.ids[0] = c.ids[1];
    tpms_config_encode(&c, bytes);
    assert(!tpms_config_decode(&loaded, bytes, sizeof(bytes))); /* valid CRC, duplicate IDs */
    memset(c.ids, 0, sizeof(c.ids));
    c.generation = UINT32_MAX;
    assert(tpms_config_assign(&c, 0, original.ids[0]));
    assert(c.generation == 0);
    tpms_config_encode(&c, bytes);
    assert(tpms_config_decode(&loaded, bytes, sizeof(bytes)));
    assert(loaded.generation == 0 && loaded.ids[1] == 0);
'''
for permutation in itertools.permutations(range(4)):
    source += "    c = original;\n"
    for wheel, sensor in enumerate(permutation):
        source += f"    tpms_config_assign(&c, {wheel}, original.ids[{sensor}]);\n"
    for wheel, sensor in enumerate(permutation):
        source += f"    assert(c.ids[{wheel}] == original.ids[{sensor}]);\n"
source += '    puts("Wheel swaps, all 24 rotations, replacements, unassignment and config corruption checks passed");\n}\n'
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / "config_test.c").write_text(source)
    executable = tmp / "config_test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(root), str(tmp / "config_test.c"), str(root / "tpms_config.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
