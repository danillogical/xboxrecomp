"""Run real copy emission through an access-counting guest-memory fixture.

The lifted `rep movs` may take a host memcpy only for a forward copy whose
ranges do not overlap and touch no hooked device window. Everything else goes
one volatile element at a time, which is the only form the MMIO hook can
emulate. The window test is the runtime's own `recomp_range_is_mmio`, read out
of templates/runtime/recomp_types.h so the fixture cannot drift from it.
"""
import re
from pathlib import Path

from tools.recomp.lifter import Lifter
from tools.recomp.disasm import Instruction

RUNTIME_HEADER = (Path(__file__).resolve().parents[2]
                  / "templates" / "runtime" / "recomp_types.h")


def _runtime_window_test():
    """The window constants and recomp_range_is_mmio, verbatim from the header."""
    text = RUNTIME_HEADER.read_text(encoding="utf-8")
    defines = re.findall(r"^#define RECOMP_MMIO_\w+\s+0x[0-9A-Fa-f]+u\s*$", text, re.M)
    body = re.search(r"^static inline int recomp_range_is_mmio\(.*?^\}$", text, re.M | re.S)
    assert len(defines) == 4 and body, "recomp_range_is_mmio contract not found"
    return "\n".join(defines) + "\n" + body.group(0) + "\n"


def test_mmio_overlap_and_direction_at_every_element_width():
    prelude = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint32_t esi,edi,ecx;
int g_df;
unsigned accesses;
uint8_t mem[131076],refmem[131076];
unsigned slot(uint32_t a){return (a&0xffffu)+(a>=0xFD000000u?65536u:0u);}
void* touch(uint32_t a){accesses++;return mem+slot(a);}
#define XBOX_PTR(a) (mem+slot(a))
#define MEM8(a) (*(volatile uint8_t*)touch(a))
#define MEM16(a) (*(volatile uint16_t*)touch(a))
#define MEM32(a) (*(volatile uint32_t*)touch(a))
''' + _runtime_window_test()
    functions = []
    for size in (1, 2, 4):
        text = {1: 'rep movsb', 2: 'rep movsw', 4: 'rep movsd'}[size]
        functions.append(f'void copy_{size}(void){{'
                         + ''.join(Lifter().lift_instruction(Instruction(0, 2, text, '', '')))
                         + '}')
    main = r'''
/* Independent of the header: does [a, a+len) touch a device window or wrap? */
static int touches_window(uint32_t a, uint64_t len){
    uint64_t end=(uint64_t)a+len;
    if(len==0) return 0;
    if(end>0x100000000ull) return 1;
    if(a<0xFE000000u && end>0xFD000000u) return 1;
    if(a<0xFE880000u && end>0xFE800000u) return 1;
    return 0;
}
int main(void){
    void(*copies[])(void)={copy_1,copy_2,copy_4};
    const unsigned sizes[]={1,2,4};
    for(unsigned w=0;w<3;w++) for(unsigned mode=0;mode<17;mode++) for(unsigned n=0;n<=8;n++){
        unsigned z=sizes[w];
        uint32_t s=0x100,d=0x400;
        int df=0;
        if(mode==1)d=s+z;                    /* forward propagation */
        if(mode==2){s+=8*z;d=s-z;df=1;}     /* backward overlap */
        if(mode==3)s=0xFD000100;            /* NV2A window */
        if(mode==4)d=0xFD000400;
        if(mode==5){s=0xFD000100;d=0xFD000400;}
        if(mode==6)s=0xFD000000-2*z;        /* range crosses into NV2A */
        if(mode==7)d=0xFD000000-2*z;
        if(mode==8){s=0xFFFFFFF0u;d=0xFD000400;} /* address wrap */
        if(mode==9){s=0xFD000100;d=0xFFFFFFF0u;}
        if(mode==10){s=0xFD000100+8*z;d=0xFD000400+8*z;df=1;}
        if(mode==11)d=s;                    /* same address */
        if(mode==12)s=0xFD000000-n*z;       /* ends at the window */
        if(mode==13)d=0xFD000000-n*z;
        if(mode==14)s=0xFE800100;           /* APU window */
        if(mode==15)d=0xFE87FFFC-2*z;       /* range crosses out of APU */
        if(mode==16){s=0xFE000100;d=0xFE700400;} /* between windows: RAM */
        for(unsigned i=0;i<sizeof mem;i++)mem[i]=refmem[i]=(uint8_t)(i*37+13);
        uint32_t rs=s,rd=d;
        for(unsigned i=0;i<n;i++){
            uint8_t element[4];
            memcpy(element,refmem+slot(rs),z);memcpy(refmem+slot(rd),element,z);
            rs+=df?-(int)z:z;rd+=df?-(int)z:z;
        }
        esi=s;edi=d;ecx=n;g_df=df;accesses=0;copies[w]();
        uint8_t *hs=mem+slot(s),*hd=mem+slot(d);
        int fast=!df && !touches_window(s,(uint64_t)n*z) && !touches_window(d,(uint64_t)n*z)
                && (hd+n*z<=hs || hs+n*z<=hd);
        if(memcmp(mem,refmem,sizeof mem) || esi!=rs || edi!=rd || ecx!=0 || accesses!=(fast?0:2*n)){
            printf("size %u mode %u count %u access %u\n",z,mode,n,accesses);return 1;
        }
    }
    return 0;
}
'''
    ran = _build_and_run(prelude + '\n'.join(functions) + main)
    assert ran.returncode == 0, ran.stdout + ran.stderr


def _build_and_run(source):
    import shutil
    import subprocess
    import tempfile
    import pytest
    cc = shutil.which("cl") or shutil.which("clang") or shutil.which("gcc")
    if not cc:
        pytest.skip("C compiler unavailable")
    with tempfile.TemporaryDirectory(prefix="recomp-regression-") as tmp:
        c, exe = Path(tmp) / "fixture.c", Path(tmp) / "fixture.exe"
        c.write_text(source, encoding="utf-8")
        args = ([cc, "/nologo", "/W0", "/O2", str(c), "/Fe:" + str(exe)]
                if Path(cc).stem.lower() == "cl"
                else [cc, "-w", "-O2", str(c), "-o", str(exe)])
        built = subprocess.run(args, cwd=tmp, capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr + source
        return subprocess.run([str(exe)], cwd=tmp, capture_output=True, text=True)
