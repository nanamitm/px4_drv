"""Build a DT295-only telemetry derivative of exactly mAgicVISION 0.1.1; no USB access."""
import argparse,hashlib,json,struct,subprocess
from pathlib import Path
HERE=Path(__file__).resolve().parent
ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--input',type=Path,required=True);ap.add_argument('--out',type=Path,required=True)
ap.add_argument('--success-limit',type=int,choices=(64,128,256),default=64)
ap.add_argument('--tool-root',default='/var/tmp/px4-fr30-binutils-2.43.1/build')
a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
def require(condition,message):
 if not condition:raise ValueError(message)
raw=bytearray(a.input.read_bytes());sha=lambda x:hashlib.sha256(x).hexdigest()
require(sha(raw)=='0b2147e190afc7626165594fdedbedc6461c0e1f044b17742123b77fc5026fcc','input is not the verified mAgicVISION 0.1.1 image')
get=lambda x:struct.unpack_from('>I',raw,x)[0]
body_off,body_len,res_off,res_len=get(24),get(28),get(40),get(44)
require(body_len==1552 and res_len==262144 and raw[:4]==b'SBC2','unexpected input layout')
def linux(p):return subprocess.check_output(['wsl','-d','Ubuntu','--','wslpath','-a',p.resolve().as_posix()],text=True).strip()
def run(tool,*args):return subprocess.check_output(['wsl','-d','Ubuntu','--',a.tool_root+'/'+tool,*map(str,args)])
run('gas/as-new','--defsym',f'DRAIN_LIMIT={a.success_limit}','-o',linux(out/'telemetry.o'),linux(HERE/'telemetry.s'))
run('ld/ld-new','-T',linux(HERE/'telemetry.ld'),'-o',linux(out/'telemetry.elf'),linux(out/'telemetry.o'))
run('binutils/objcopy','-O','binary',linux(out/'telemetry.elf'),linux(out/'telemetry.bin'))
(out/'telemetry.asm').write_bytes(run('binutils/objdump','-d',linux(out/'telemetry.elf')))
nm=run('binutils/nm-new','-n',linux(out/'telemetry.elf')).decode()
symbols={line.split()[2]:int(line.split()[0],16) for line in nm.splitlines() if len(line.split())==3}
code=(out/'telemetry.bin').read_bytes();require(len(code)<=496,'telemetry exceeds reserved bootstrap capacity')
resident=bytearray(raw[res_off:res_off+res_len]);base=0x80380000
original=bytes(resident[0x1986c:0x1986c+44])
require(original[26:28]==bytes.fromhex('ea08') and original[36:44]==bytes.fromhex('e203c401aa18e3f4'),'unexpected original loop branches')
expected=bytearray(original)
if a.success_limit==128:expected[38:40]=bytes.fromhex('c801')
if a.success_limit==256:
 expected=original[:26]+bytes.fromhex('ea09')+original[28:36]+bytes.fromhex('e2049b010100')+original[40:42]+bytes.fromhex('e3f3')
loop_start=symbols['telemetry_drain']-0xe1610
require(code[loop_start:loop_start+len(expected)]==expected,'drain loop changed outside allowed success-limit encoding')
sites=[(0x80381656,0x80381d0c,'telemetry_dispatch'),(0x8039942e,0x8039986c,'telemetry_drain'),(0x8039975e,0x8038328c,'telemetry_fifo')]
for address,old,name in sites:
 offset=address-base;require(struct.unpack_from('>I',resident,offset)[0]==old,'unexpected patch site contents')
 struct.pack_into('>I',resident,offset,symbols[name])
resident[-8:]=b'DTS1.001' if a.success_limit==64 else f'DTS1.{a.success_limit:03d}'.encode('ascii')
struct.pack_into('>I',resident,0x2fff8,0)
sum32=sum(struct.unpack('>49152I',resident[:0x30000])) & 0xffffffff
struct.pack_into('>I',resident,0x2fff8,(-sum32)&0xffffffff)
require(sum(struct.unpack('>49152I',resident[:0x30000]))&0xffffffff==0,'checksum failure')
raw[res_off:res_off+res_len]=resident
new_body=raw[body_off:body_off+body_len]+code
new_off=len(raw);raw.extend(new_body)
struct.pack_into('>II',raw,24,new_off,len(new_body))
struct.pack_into('>I',raw,12,len(raw))
(out/'magicvision-dt295-telemetry.bin').write_bytes(raw)
(out/'resident-telemetry.bin').write_bytes(resident)
manifest={'schema':1,'target':'PIX-DT295 only','input_sha256':sha(a.input.read_bytes()),'output_sha256':sha(raw),'resident_sha256':sha(resident),'success_limit':a.success_limit,'attempt_limit':4096,'telemetry_bytes':len(code),'body_bytes':len(new_body),'body_capacity':2048,'entry':'0x000e1610','symbols':symbols,'patch_sites':[{'address':hex(addr),'original':hex(old),'replacement':hex(symbols[name])} for addr,old,name in sites],'source_sha256':{name:sha((HERE/name).read_bytes()) for name in ('telemetry.s','telemetry.ld','build.py')},'hardware_tested':False,'independent_firmware':False}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps(manifest,indent=2))
