"""Fixed data-only MASM import and strict prelaunch PE checks; development only."""
import argparse
import os
from pathlib import Path
import stat
import struct

ROOT=Path(__file__).resolve().parents[1]
DLL='api-ms-win-crt-math-l1-1-0.dll'
ASSEMBLY='OPTION CASEMAP:NONE\nag2 SEGMENT ALIGN(4) READ WRITE ALIAS(".idata$2") \'DATA\'\nPUBLIC __IMPORT_DESCRIPTOR_airack_saturation_release\n__IMPORT_DESCRIPTOR_airack_saturation_release LABEL DWORD\n    DD IMAGEREL ag_lookup\n    DD 0, 0\n    DD IMAGEREL ag_dll\n    DD IMAGEREL __imp_airack_saturation_release_pow\nag2 ENDS\nag3 SEGMENT ALIGN(4) READ WRITE ALIAS(".idata$3") \'DATA\'\nag_null_descriptor DD 5 DUP(0)\nag3 ENDS\nag4 SEGMENT ALIGN(8) READ WRITE ALIAS(".idata$4") \'DATA\'\nag_lookup LABEL QWORD\n    DD IMAGEREL ag_hint\n    DD 0\n    DQ 0\nag4 ENDS\nag5 SEGMENT ALIGN(8) READ WRITE ALIAS(".idata$5") \'DATA\'\nPUBLIC __imp_airack_saturation_release_pow\n__imp_airack_saturation_release_pow LABEL QWORD\n    DD IMAGEREL ag_hint\n    DD 0\n    DQ 0\nag5 ENDS\nag6 SEGMENT ALIGN(2) READ WRITE ALIAS(".idata$6") \'DATA\'\nag_hint DW 0\n    DB "pow", 0\n    ALIGN 2\nag_dll DB "api-ms-win-crt-math-l1-1-0.dll", 0\nag6 ENDS\nEND\n'

def require(ok,message):
    if not ok:raise ValueError(message)

def validate_assembly(raw):
    require(raw.replace(b'\r\n',b'\n')==ASSEMBLY.encode('ascii'),'fixed data-only import assembly changed')

def plain_output(path):
    # Check lexical prefixes before resolving: junction/.. must not erase a reparse point.
    path=Path(path)
    if not path.is_absolute():path=Path.cwd()/path
    for ancestor in reversed((path,*path.parents)):
        try:info=ancestor.lstat()
        except FileNotFoundError:continue
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info,'st_file_attributes',0)&0x400,
                'redirected output path')
    path=path.resolve();root=ROOT.resolve();build=(root/'build').resolve()
    require(path!=root and (root not in path.parents or build in path.parents),
            'output must be outside source/corpus roots or inside build')
    require(not path.exists(),'fresh output required')
    return path

def inspect_pe(raw):
    """Read only AMD64 PE32+ imports; never run a malformed trial image."""
    def unpack(form,off):
        size=struct.calcsize('<'+form)
        require(0<=off and off+size<=len(raw),'truncated PE data')
        return struct.unpack_from('<'+form,raw,off)
    require(raw[:2]==b'MZ','DOS header missing')
    pe,=unpack('I',0x3c);require(raw[pe:pe+4]==b'PE\0\0','PE signature missing')
    machine,count,_,_,_,opt_size,_=unpack('HHIIIHH',pe+4)
    require(machine==0x8664 and 0<count<=96 and opt_size>=240,'AMD64 PE headers required')
    opt=pe+24;require(unpack('H',opt)[0]==0x20b and unpack('I',opt+108)[0]>=13,'PE32+ directories missing')
    imports=unpack('II',opt+120);iat=unpack('II',opt+208)
    require(imports[0]>0 and imports[1]>=40 and imports[1]%20==0,'Import Directory missing/malformed')
    require(iat[0]>0 and iat[1]>=16 and iat[1]%8==0,'IAT Directory missing/malformed')
    sections=[]
    for i in range(count):
        at=opt+opt_size+i*40
        virtual_size,rva,size,offset=unpack('IIII',at+8)
        require(offset+size<=len(raw),'truncated section')
        sections.append((rva,size,offset))
    def offset(rva,size=1):
        matches=[base+rva-start for start,length,base in sections if start<=rva and rva+size<=start+length]
        require(len(matches)==1,'unmapped or ambiguous RVA');return matches[0]
    def string(rva):
        at=offset(rva);end=raw.find(b'\0',at,at+4096)
        require(end>=at,'unterminated import name');offset(rva,end-at+1)
        try:return raw[at:end].decode('ascii')
        except UnicodeError as exc:raise ValueError('non-ASCII import name') from exc
    offset(*imports);offset(*iat)
    descriptors=[];seen_null=False
    for n in range(imports[1]//20):
        lookup,stamp,chain,name,first=unpack('IIIII',offset(imports[0]+n*20,20))
        if not any((lookup,stamp,chain,name,first)):
            seen_null=True;continue
        require(not seen_null,'live descriptor follows null')
        require(lookup and name and first and stamp==0 and chain==0,'unsupported import descriptor')
        dll=string(name);functions=[]
        for j in range(65536):
            entry,=unpack('Q',offset(lookup+j*8,8));initial,=unpack('Q',offset(first+j*8,8))
            require(entry==initial,'initial ILT/IAT disagreement')
            require(iat[0]<=first+j*8 and first+(j+1)*8<=iat[0]+iat[1],'descriptor outside IAT Directory')
            if not entry:break
            if entry & (1<<63):
                require(entry & 0x7fffffffffff0000==0,'reserved ordinal import bits')
                functions.append(dict(ordinal=entry&0xffff,iat_rva=first+j*8))
            else:
                require(entry<2**32,'invalid named import RVA')
                hint,=unpack('H',offset(entry,2));functions.append(dict(name=string(entry+2),hint=hint,iat_rva=first+j*8))
        else:raise ValueError('unterminated import thunk')
        require(functions,'empty live import descriptor')
        descriptors.append(dict(dll=dll,lookup_rva=lookup,iat_rva=first,functions=functions))
    require(seen_null and descriptors,'missing descriptor terminator')
    selected=[d['functions'][0] for d in descriptors if d['dll'].lower()==DLL and len(d['functions'])==1
              and d['functions'][0].get('name')=='pow' and d['functions'][0].get('hint')==0]
    require(len(selected)==1,'one private hint0 single-entry release pow descriptor required')
    return dict(import_directory=dict(rva=imports[0],bytes=imports[1]),iat_directory=dict(rva=iat[0],bytes=iat[1]),
                descriptors=descriptors,private_pow_iat_rva=selected[0]['iat_rva'])

def inspect_caller(raw,proof):
    # Narrow x64 RIP-relative IAT call/jump witness, paired with actual COFF relocation/disassembly.
    pe=struct.unpack_from('<I',raw,0x3c)[0];count=struct.unpack_from('<H',raw,pe+6)[0]
    section=pe+24+struct.unpack_from('<H',raw,pe+20)[0];calls=[]
    for i in range(count):
        at=section+i*40;rva,size,base=struct.unpack_from('<III',raw,at+12)
        flags=struct.unpack_from('<I',raw,at+36)[0]
        if not flags&0x20000000:continue
        code=raw[base:base+size]
        for j in range(max(0,len(code)-5)):
            if code[j:j+2] in (b'\xff\x15',b'\xff\x25'):
                target=rva+j+6+struct.unpack_from('<i',code,j+2)[0]
                if target==proof['private_pow_iat_rva']:calls.append(rva+j)
    require(calls,'no executable RIP-relative private IAT call/jump')
    return calls

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assembly',type=Path);parser.add_argument('--image',type=Path)
    args=parser.parse_args()
    try:
        require(bool(args.assembly)!=bool(args.image),'select assembly or image')
        if args.assembly:validate_assembly(args.assembly.read_bytes())
        else:
            import json
            raw=args.image.read_bytes();proof=inspect_pe(raw)
            proof['private_iat_call_rvas']=inspect_caller(raw,proof)
            print(json.dumps(proof,indent=2))
    except (ValueError,OSError) as exc:parser.exit(1,str(exc)+'\n')

if __name__=='__main__':main()
