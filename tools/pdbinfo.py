import struct,sys,os
p=sys.argv[1]
d=open(p,'rb').read()
pe=struct.unpack_from('<I',d,0x3C)[0]
nsec=struct.unpack_from('<H',d,pe+6)[0]
optsz=struct.unpack_from('<H',d,pe+20)[0]
opt=pe+24
magic=struct.unpack_from('<H',d,opt)[0]
ddoff=opt+(112 if magic==0x20b else 96)
dd_num=ddoff-opt+4
sect=opt+optsz
secs=[]
for i in range(nsec):
    o=sect+i*40
    name=d[o:o+8].rstrip(b'\0').decode()
    vsize,vaddr,rawsz,rawptr=struct.unpack_from('<IIII',d,o+8)
    secs.append((vaddr,vsize,rawptr,rawsz,name))
def r2o(rva):
    for vaddr,vsize,rawptr,rawsz,name in secs:
        if vaddr<=rva<vaddr+max(vsize,rawsz): return rawptr+(rva-vaddr)
    return None
dbg_rva,dbg_sz=struct.unpack_from('<II',d,ddoff+6*8)
print('debug rva',hex(dbg_rva),'size',dbg_sz)
o=r2o(dbg_rva)
n=dbg_sz//28
for i in range(n):
    ent=o+i*28
    typ,sz,data_rva,data_ptr=struct.unpack_from('<IIII',d,ent+12)
    if typ==2:
        pdb=struct.unpack_from('<I',d,data_ptr)[0]
        guid=struct.unpack_from('<IHH',d,data_ptr+4)
        age=struct.unpack_from('<I',d,data_ptr+20)[0]
        g2=struct.unpack_from('<8s',d,data_ptr+4)[0]
        print('PDB age',age)
        print('GUID',g2.hex())
        # convert to canonical
        import uuid
        g=struct.unpack_from('<IHH',d,data_ptr+4)
        print('GUIDSTR',('%08X%04X%04X'%(g[0],g[1],g[2]))+g2[8:].hex().upper(), 'AGE',age)
