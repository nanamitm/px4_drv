"""Limited FR instruction model for the telemetry shim; no USB/interrupt/cache emulation."""
import argparse,json,struct
from pathlib import Path
BASE=0xe1610;STOP=0xfffffffe
class Model:
 def __init__(self,code):
  self.code=bytearray(code);self.r=[0x11000000+i for i in range(16)];self.r[15]=0x210000;self.rp=STOP;self.eq=False;self.lt=False;self.ult=False;self.hooks={};self.steps=0
  self.mem={BASE:self.code,0x200000:bytearray(0x11000),0x1902020:bytearray(16)};self.writes=[]
 def load(self,a,n=4):
  assert a%n==0 or BASE<=a<BASE+len(self.code)
  for b,d in self.mem.items():
   if b<=a and a+n<=b+len(d):return int.from_bytes(d[a-b:a-b+n],'big')
  raise AssertionError(f'unmapped read {a:x}')
 def store(self,a,v):
  assert a%4==0
  for b,d in self.mem.items():
   if b<=a and a+4<=b+len(d):d[a-b:a-b+4]=(v&0xffffffff).to_bytes(4,'big');self.writes.append(a);return
  raise AssertionError(f'unmapped write {a:x}')
 def signed(self,v):return v if v<0x80000000 else v-0x100000000
 def cmp(self,a,b):self.eq=a==b;self.lt=self.signed(a)<self.signed(b);self.ult=a<b
 def push(self,v):self.r[15]-=4;self.store(self.r[15],v)
 def pop(self):v=self.load(self.r[15]);self.r[15]+=4;return v
 def subcall(self,pc):
  rp=self.rp;self.rp=STOP;self.execute(pc);self.rp=rp
 def execute(self,pc):
  r=self.r
  while pc!=STOP:
   self.steps+=1;assert self.steps<1000000
   if pc in self.hooks:self.hooks[pc](self);pc=self.rp;continue
   ins=self.load(pc,2);pc+=2;t=ins>>8;i=(ins>>4)&15;j=ins&15
   if ins==0x9720:pc=self.rp
   elif ins&0xfff0==0x9710:self.rp,pc=pc,r[j]
   elif ins&0xfff0==0x9700:pc=r[j]
   elif ins&0xfff0==0x9f80:r[j]=self.load(pc);pc+=4
   elif t==0x9b:r[j]=i*65536+self.load(pc,2);pc+=2
   elif ins&0xf000==0xc000:r[j]=(ins>>4)&255
   elif t==0x8b:r[j]=r[i]
   elif t==0x04:r[j]=self.load(r[i])
   elif t==0x14:self.store(r[i],r[j])
   elif t==0x17 and i==0:self.push(r[j])
   elif ins==0x1781:self.push(self.rp)
   elif t==0x07 and i==0:r[j]=self.pop()
   elif ins==0x0781:self.rp=self.pop()
   elif t==0x03:r[j]=self.load(r[15]+i*4)
   elif t==0x13:self.store(r[15]+i*4,r[j])
   elif t==0x8f:
    for bit in range(8):
     if ins&(1<<bit):self.push(r[15-bit])
   elif t==0x8d:
    for bit in range(8):
     if ins&(1<<bit):r[8+bit]=self.pop()
   elif t==0xa3:r[15]+=((ins&255)-256 if ins&128 else ins&255)*4
   elif t==0xa0:r[j]=(r[j]+i)&0xffffffff
   elif t==0xa1:r[j]=(r[j]+i-16)&0xffffffff
   elif t==0xa2:r[j]=(r[j]+r[i])&0xffffffff
   elif t==0xae:r[j]=(r[j]-r[i])&0xffffffff
   elif t==0xa8:self.cmp(r[j],i)
   elif t==0xaa:self.cmp(r[j],r[i])
   elif t in (0xe0,0xe2,0xe3,0xea,0xeb):
    take=t==0xe0 or t==0xe2 and self.eq or t==0xe3 and not self.eq or t==0xea and self.lt or t==0xeb and not self.lt
    if take:pc+=((ins&255)-256 if ins&128 else ins&255)*2
   else:raise AssertionError(f'unsupported {ins:04x} at {pc-2:x}')

def case(code,sym,sequence,expected,command=0x24,table=0x803adfe4):
 m=Model(code);r=m.r;r[4]=0xabc;r[5]=table;r[6]=0;r[7]=0x200000
 m.store(r[7],0x585a4431);m.store(r[7]+12,command)
 for a,v in [(0x1902024,0x30000),(0x1902028,0x10000),(0x190202c,0x8000)]:m.store(a,v)
 initial=r.copy();n=0;pack_n=0;last=0
 def pack(m):
  nonlocal pack_n
  assert m.r[4:8]==[0xabc,0x138ec00,0x30000,0]
  assert m.load(m.r[15])==0x200100 and m.load(m.r[15]+4)==0x200104
  pack_n+=1;m.r[4]=last & 0xffffffff
 def step(m):
  nonlocal n,last
  last=sequence[min(n,len(sequence)-1)];n+=1
  if last==2:m.r[4]=1;return # no input; no FIFO call
  m.r[15]-=8;m.store(m.r[15],0x200100);m.store(m.r[15]+4,0x200104)
  m.r[4:8]=[0xabc,0x138ec00,0x30000,0]
  m.subcall(sym['telemetry_fifo']);m.r[15]+=8
 def generic(m):
  assert m.r[4:8]==initial[4:8]
  if command==0x24 and table==0x803adfe4:
   m.r[4]=0xabc;m.subcall(sym['telemetry_drain'])
   # Real dispatcher may overwrite response payload. Scratch must survive it.
   for a in range(0x200018,0x200040,4):m.store(a,0xdeadbeef)
   m.mem[0x1902020][8:16]=(0x14000).to_bytes(4,'big')+(0xc000).to_bytes(4,'big')
  m.r[4]=0x12345678
 m.hooks={0x80381d0c:generic,0x80399668:step,0x8038328c:pack}
 m.writes.clear();m.execute(sym['telemetry_dispatch'])
 assert m.r[4]==0x12345678 and m.r[8:]==initial[8:] and m.rp==STOP
 assert m.load(sym['telemetry_pointer'])==0
 if command==0x24 and table==0x803adfe4:
  data=[m.load(0x200018+i*4) for i in range(10)]
  assert data==[0x44545331,0x10000,0x8000,0x14000,0xc000,*expected[:3],0x30000,expected[3]],data
 else:assert n==0 and pack_n==0 and m.load(0x20000c)==command
 # MMIO is read-only; only packet, stack and the temporary bootstrap pointer are written.
 assert all(0x200000<=a<=0x210000 or a==sym['telemetry_pointer'] for a in m.writes)
 return {'attempts':n,'fifo_calls':pack_n,'model_steps':m.steps}

def main():
 ap=argparse.ArgumentParser();ap.add_argument('build',type=Path);a=ap.parse_args()
 manifest=json.loads((a.build/'manifest.json').read_text());code=(a.build/'telemetry.bin').read_bytes();s=manifest['symbols']
 limit=manifest.get('success_limit',64)
 results={
 'success_limit':case(code,s,[0],[limit,limit,0,limit]),
 'busy_then_success':case(code,s,[1]*16+[0]*limit,[16+limit,limit,16,16+limit]),
 'no_input_then_success':case(code,s,[2]*100+[0]*limit,[100+limit,limit,0,limit]),
 '4096_busy_budget':case(code,s,[1],[4096,0,4096,4096]),
 '4096_no_input_budget':case(code,s,[2],[4096,0,0,0]),
 'negative_first':case(code,s,[-5],[1,0,0,1]),
 'negative_after_success':case(code,s,[0]*7+[-5],[8,7,0,8]),
 'other_command':case(code,s,[0],[],command=0x25),
 'other_target':case(code,s,[0],[],table=0x8039f8a0)}
 (a.build/'model-tests.json').write_text(json.dumps(results,indent=2)+'\n');print(json.dumps(results,indent=2))
if __name__=='__main__':main()
