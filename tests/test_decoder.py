#!/usr/bin/env python3
"""Replay real SDR pulse runs through the standalone app's C decoder."""
import collections
import ctypes
import json
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

class Reading(ctypes.Structure):
    _fields_ = [("id",ctypes.c_uint32),("pressure_raw",ctypes.c_uint8),
                ("temperature_c",ctypes.c_int16),("state",ctypes.c_uint8),
                ("flags",ctypes.c_uint8),("repeat",ctypes.c_uint8),("unknown",ctypes.c_uint8)]

def pulse_blocks(text):
    block=None
    for line in text.splitlines():
        if line.startswith(';fsk '): block=[]
        elif line==';end':
            if block: yield block
            block=None
        elif block is not None and line and not line.startswith((';','{')):
            parts=line.split()
            if len(parts)==2 and all(p.isdigit() for p in parts):
                high,low=map(int,parts)
                block.extend([(True,high),(False,low)])

def signature(r):
    return (f'{r.id:08x}',r.pressure_raw,r.temperature_c,r.state,r.flags,r.repeat,r.unknown)

def reference(r):
    return (r['id'],round(r['pressure_kPa']/1.375),int(r['temperature_C']),
            r['state'],r['flags'],r['repeat'],r['maybe_battery'])

def frame(raw):
    return '10101010101010110'+''.join('10' if b&(1<<i) else '01' for b in raw for i in range(7,-1,-1))

def runs(bits):
    last=bits[0]; count=0; result=[]
    for bit in bits:
        if bit!=last:
            result.append((last=='1',count*52)); last=bit; count=0
        count+=1
    result.append((last=='1',count*52))
    return result

def main():
    captures=Path(sys.argv[1]) if len(sys.argv)>1 else ROOT.parent/'session-01'
    with tempfile.TemporaryDirectory() as tmp:
        temp=Path(tmp)
        (temp/'host.c').write_text('''#include "hyundai_decoder.h"
static HyundaiDecoder decoder;
void reset(void) { hyundai_decoder_reset(&decoder); }
bool feed(bool level, uint32_t duration, HyundaiReading* r) {
 return hyundai_decoder_pulse(&decoder,level,duration,r);
}
''')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC',
                        '-I',str(ROOT),str(temp/'host.c'),str(ROOT/'hyundai_decoder.c'),
                        '-o',str(temp/'decoder.so')],check=True)
        lib=ctypes.CDLL(str(temp/'decoder.so'))
        lib.feed.argtypes=[ctypes.c_bool,ctypes.c_uint32,ctypes.POINTER(Reading)]
        lib.feed.restype=ctypes.c_bool
        lib.hyundai_pressure_psi.argtypes=[ctypes.c_uint8]
        lib.hyundai_pressure_psi.restype=ctypes.c_float
        def decode(pulses, reset=True):
            if reset: lib.reset()
            found=[]
            for level,duration in pulses:
                result=Reading()
                if lib.feed(level,duration,ctypes.byref(result)): found.append(signature(result))
            return found
        raw=bytes.fromhex('201122334409a04a2172')
        valid=frame(raw)
        expected=('11223344',160,24,32,0,9,33)
        assert decode(runs(valid))==[expected]
        assert decode(runs(''.join('0' if b=='1' else '1' for b in valid)))==[expected]
        assert abs(lib.hyundai_pressure_psi(160)-31.9083)<.001
        for length in range(1,len(valid)):
            assert not decode(runs(valid[:length])),('truncated',length)
        for bit in range(80):
            damaged=bytearray(raw); damaged[bit//8]^=1<<(7-bit%8)
            assert not decode(runs(frame(damaged))),('corrupted',bit)
        invalid=list(valid); invalid[17:19]=['0','0']
        assert not decode(runs(''.join(invalid)))
        partial=runs(valid[:80])
        for gap in (0,10,1000,0xffffffff):
            assert decode(partial+[(True,gap)]+runs(valid))==[expected]
        assert decode(runs(valid)+[(False,1000)]+runs(valid))==[expected,expected]
        # Queue discontinuity: the app explicitly resets before the next sample.
        assert not decode(runs(valid[:80]))
        lib.reset()
        assert not decode(runs(valid[80:]),reset=False)
        assert decode(runs(valid))==[expected]
        rng=random.Random(433)
        for _ in range(500):
            assert not decode(runs(''.join(rng.choice('01') for _ in range(300))))
        print('PASS: streaming pulse decode, both polarities, truncations, 80 corrupt bits, Manchester, gaps, dropped data, repeated packets and noise')
        if '--synthetic-only' in sys.argv[1:]:
            return
        files=sorted(captures.glob('*.cu8'))
        assert files,'No captures found'
        total=0; positive=0
        for file in files:
            output=subprocess.run(['rtl_433','-c','0','-s','1000000','-R','186','-r',str(file),
                                   '-F','json','-w','ook:-'],capture_output=True,text=True,timeout=30,check=True).stdout
            refs=[json.loads(line) for line in output.splitlines() if line.startswith('{') and '"Hyundai-VDO"' in line]
            expected_counts=collections.Counter(reference(r) for r in refs)
            actual=[]; inverted=[]
            for pulses in pulse_blocks(output):
                actual.extend(decode(pulses))
                inverted.extend(decode([(not level,duration) for level,duration in pulses]))
            assert collections.Counter(actual)==expected_counts,(file.name,expected_counts,collections.Counter(actual))
            assert collections.Counter(inverted)==expected_counts,(file.name,'polarity')
            total+=len(actual); positive+=bool(refs)
        print(f'PASS: {total} packets, {positive} positive recordings, all {len(files)} recordings match rtl_433 with both polarities')

if __name__=='__main__': main()
