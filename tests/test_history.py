#!/usr/bin/env python3
"""Check cache expiry and corruption handling using the actual portable C codec."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include "tpms_history.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void) {
    assert(tpms_cache_recent(100, 100));
    assert(tpms_cache_recent(100, 1899));
    assert(!tpms_cache_recent(100, 1900));
    assert(!tpms_cache_recent(100, 86500));
    assert(!tpms_cache_recent(100, 99));
    assert(!tpms_cache_recent(0, 100));
    TpmsHistory original={.generation=42};
    for(unsigned i=0;i<16;i++) {
        TpmsSaved entry={.reading={.id=0x11000000+i,.pressure_raw=160,
            .temperature_c=(int16_t)(i?205:-50),.state=32,.flags=15,.repeat=9,.unknown=255},
            .timestamp=1700000000+i,.frames=240+i};
        tpms_history_update(&original,&entry);
    }
    uint8_t bytes[TPMS_HISTORY_BYTES], broken[TPMS_HISTORY_BYTES];
    size_t n=tpms_history_encode(&original,bytes);
    assert(n==TPMS_HISTORY_BYTES);
    TpmsHistory loaded={0};
    assert(tpms_history_decode(&loaded,bytes,n));
    assert(loaded.generation==42 && loaded.count==16);
    assert(loaded.sensors[0].reading.temperature_c==-50);
    assert(loaded.sensors[15].reading.temperature_c==205);
    assert(loaded.sensors[0].reading.pressure_raw==160);
    assert(loaded.sensors[15].frames==255);
    for(size_t i=0;i<n;i++) {
        memcpy(broken,bytes,n); broken[i]^=1;
        TpmsHistory untouched=loaded;
        assert(!tpms_history_decode(&loaded,broken,n));
        assert(!memcmp(&loaded,&untouched,sizeof(loaded)));
    }
    for(size_t i=0;i<n;i++) assert(!tpms_history_decode(&loaded,bytes,i));
    assert(!tpms_history_decode(&loaded,bytes,n+1));
    TpmsSaved update=original.sensors[3]; update.frames=1000; update.timestamp=1700001000;
    tpms_history_update(&original,&update);
    assert(original.count==16 && original.sensors[3].frames==1000);
    TpmsSaved replacement=update; replacement.reading.id=0xbadf00d;
    tpms_history_update(&original,&replacement);
    assert(original.count==16 && original.sensors[0].reading.id==0xbadf00d);
    /* Even a checksum-valid state must reject impossible fields and duplicates. */
    original.sensors[0].reading.flags=16;
    n=tpms_history_encode(&original,bytes); assert(!tpms_history_decode(&loaded,bytes,n));
    original.sensors[0].reading.flags=0;
    original.sensors[0].reading.id=original.sensors[1].reading.id;
    n=tpms_history_encode(&original,bytes); assert(!tpms_history_decode(&loaded,bytes,n));
    puts("Cache expiry, full round trip, truncation, corruption and invalid-state checks passed");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    (tmp / "history_test.c").write_text(source)
    executable = tmp / "history_test"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(root), str(tmp / "history_test.c"), str(root / "tpms_history.c"),
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
