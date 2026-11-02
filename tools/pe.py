import struct
class PE:
    def __init__(self,path):
        self.d=open(path,'rb').read()
        d=self.d
        self.pe=struct.unpack_from('<I',d,0x3C)[0]
        self.machine=struct.unpack_from('<H',d,self.pe+4)[0]
        nsec=struct.unpack_from('<H',d,self.pe+6)[0]
        optsz=struct.unpack_from('<H',d,self.pe+20)[0]
        opt=self.pe+24; self.opt=opt
        magic=struct.unpack_from('<H',d,opt)[0]
        self.is64= magic==0x20b
        self.imagebase=struct.unpack_from('<Q',d,opt+24)[0]
        self.sizeofimage=struct.unpack_from('<I',d,opt+56)[0]
        self.ddoff= opt+(112 if self.is64 else 96)
        self.sections=[]
        s=opt+optsz
        for i in range(nsec):
            o=s+i*40
            name=d[o:o+8].rstrip(b'\0').decode('latin1')
            vsize,vaddr,rawsz,rawptr=struct.unpack_from('<IIII',d,o+8)
            ch=struct.unpack_from('<I',d,o+36)[0]
            self.sections.append(dict(name=name,va=vaddr,vsize=vsize,rawsz=rawsz,rawptr=rawptr,ch=ch))
    def dd(self,i):
        return struct.unpack_from('<II',self.d,self.ddoff+i*8)
    def r2o(self,rva):
        for s in self.sections:
            if s['va']<=rva<s['va']+max(s['vsize'],s['rawsz']):
                if rva-s['va']<s['rawsz']:
                    return s['rawptr']+(rva-s['va'])
        return None
    def o2r(self,off):
        for s in self.sections:
            if s['rawptr']<=off<s['rawptr']+s['rawsz']:
                return s['va']+(off-s['rawptr'])
        return None
    def read(self,rva,n):
        o=self.r2o(rva)
        return self.d[o:o+n]
    def cstr(self,rva):
        o=self.r2o(rva)
        if o is None: return None
        e=self.d.index(b'\0',o)
        return self.d[o:e].decode('latin1')
    def imports(self):
        rva,sz=self.dd(1)
        res=[]
        o=self.r2o(rva)
        while True:
            ent=self.d[o:o+20]
            if len(ent)<20 or ent==b'\0'*20: break
            oft,ts,fc,name_rva,ft=struct.unpack('<IIIII',ent)
            dll=self.cstr(name_rva)
            thunk=oft or ft
            j=0
            while True:
                t=self.r2o(thunk+j*(8 if self.is64 else 4))
                val=struct.unpack_from('<Q' if self.is64 else '<I',self.d,t)[0]
                if val==0: break
                if self.is64 and (val & (1<<63)):
                    res.append((dll,j,None,ft+j*8,val & 0xffff))
                else:
                    nm=self.cstr((val & 0x7fffffff)+2)
                    res.append((dll,j,nm,ft+j*8,None))
                j+=1
            o+=20
        return res
    def exports(self):
        rva,sz=self.dd(0)
        res={}
        if not rva: return res
        o=self.r2o(rva)
        nfun,nname=struct.unpack_from('<II',self.d,o+20)[0],struct.unpack_from('<II',self.d,o+24)[0]
        af,an,ao=struct.unpack_from('<III',self.d,o+28)
        fo=self.r2o(af); no=self.r2o(an); oo=self.r2o(ao)
        for i in range(nname):
            nr=struct.unpack_from('<I',self.d,no+i*4)[0]
            ordi=struct.unpack_from('<H',self.d,oo+i*2)[0]
            fr=struct.unpack_from('<I',self.d,fo+ordi*4)[0]
            res[self.cstr(nr)]=fr
        return res
    def pdata(self):
        rva,sz=self.dd(3)
        res=[]
        if not rva: return res
        o=self.r2o(rva)
        for i in range(sz//12):
            b,e,u=struct.unpack_from('<III',self.d,o+i*12)
            res.append((b,e,u))
        return res
