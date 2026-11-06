"""A helper can return flags which the caller passes straight to another call."""
from pathlib import Path
import subprocess

import pytest

from tools.conformance import __main__ as compiler, harness
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator


def test_only_boundaries_whose_far_side_reads_flags_publish(monkeypatch):
    base = 0x10000
    image = bytearray(b'\xcc' * 0x50)
    # caller: call answer; jne done; call plain; done: ret
    image[:13] = bytes.fromhex('e81b0000007505e834000000c3')
    image[0x20:0x23] = image[0x40:0x43] = bytes.fromhex('39c8c3')  # cmp eax,ecx; ret
    monkeypatch.setattr(config, '_SECTIONS', [
        config.Section('.text', base, len(image), 0, len(image), True)])
    db = {base + off: {'start': hex(base + off), 'end': base + off + size,
                       'name': name}
          for off, size, name in ((0, 13, 'caller'), (0x20, 3, 'answer'),
                                  (0x40, 3, 'plain'))}
    translator = FunctionTranslator(bytes(image), db)
    caller, answer, plain = (translator.translate_function(a, db[a]) for a in db)
    assert 'g_eflags & 0x40u' in caller
    assert 'flags across call boundary' in answer
    # The same compare whose flags nobody reads translates as before.
    assert 'g_eflags' not in plain and '_cf' not in plain


def test_flags_survive_two_calls_and_both_return_paths(tmp_path, monkeypatch):
    vcvars = compiler._find_vcvars()
    if vcvars is None:
        pytest.skip('32-bit MSVC required')
    base = 0x10000
    image = bytearray(b'\xcc' * 0x100)
    # Wrapper: call compare; call consumer; ret.
    image[:11] = bytes.fromhex('e81b000000e836000000c3')
    # compare: cmp eax,ecx; je equal; mov eax,9; ret; equal: mov eax,7; ret.
    # The flags must describe the comparison, not the overwritten register.
    compare = bytes.fromhex('39c87406b809000000c3b807000000c3')
    image[0x20:0x20 + len(compare)] = compare
    monkeypatch.setattr(config, '_SECTIONS', [
        config.Section('.text', base, len(image), 0, len(image), True)])
    cases = [('e', 0x94), ('ne', 0x95), ('b', 0x92), ('be', 0x96),
             ('a', 0x97), ('l', 0x9c), ('le', 0x9e), ('g', 0x9f),
             ('s', 0x98), ('o', 0x90), ('p', 0x9a)]
    source = [harness._PREAMBLE, '#include <assert.h>']
    for name, opcode in cases:
        # consumer: setcc al; movzx eax,al; ret.
        image[0x40:0x47] = bytes([0x0f, opcode, 0xc0, 0x0f, 0xb6, 0xc0, 0xc3])
        db = {base + off: {'start': hex(base + off), '_addr': base + off,
                           'end': base + off + size, 'name': f'{label}_{name}'}
              for off, size, label in ((0, 11, 'wrapper'),
                                       (0x20, len(compare), 'compare'), (0x40, 7, 'consumer'))}
        translator = FunctionTranslator(bytes(image), db)
        for address in reversed(db):
            code = translator.translate_function(address, db[address])
            assert '_flags /*' not in code
            source.append(code)
        source.append(f'''#pragma push_macro("eax")
        #undef eax
        static unsigned native_{name}(unsigned x, unsigned y) {{
            unsigned result;
            __asm {{
                mov eax,x
                cmp eax,y
                set{name} al
                movzx eax,al
                mov result,eax
            }}
            return result;
        }}
        #pragma pop_macro("eax")''')
    # The entry block can also have a backedge with a different flag setter.
    loop = bytes.fromhex('740349ebfb89c8c3')  # je done; dec ecx; jmp entry
    info = {'start': hex(base), '_addr': base, 'end': base + len(loop), 'name': 'entry_loop'}
    translator = FunctionTranslator(loop, {base: info})
    source.append(translator.translate_function(base, info))
    source.append('''int main(void) {
        unsigned values[] = {0,1,2,0x7fffffff,0x80000000,0xffffffff};
        unsigned i,j;
        for(i=0;i<6;i++) for(j=0;j<6;j++) {''')
    for name, _ in cases:
        source.append(f'''g_eax=values[i]; g_ecx=values[j]; g_eflags=0x8c5;
            g_esp=(uint32_t)(uintptr_t)(g_guest_stack+sizeof(g_guest_stack)-16);
            wrapper_{name}();
            assert(g_eax == native_{name}(values[i],values[j]));''')
    source.append('''}
        g_eflags=0x40; g_ecx=3; entry_loop(); assert(g_eax==3);
        g_eflags=0; g_ecx=3; entry_loop(); assert(g_eax==0);
        return 0; }''')
    (tmp_path / 'check.c').write_text('\n'.join(source), newline='\n')
    include = Path(__file__).resolve().parents[2] / 'templates/runtime'
    built = compiler._cl(vcvars, str(tmp_path), f'/O2 /I"{include}" check.c /Fecheck.exe')
    assert built.returncode == 0, built.stdout + built.stderr
    subprocess.run([str(tmp_path / 'check.exe')], check=True, timeout=30)
