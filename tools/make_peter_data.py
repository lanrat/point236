# Extract the non-code parts of a Peter-made EXE (resources + Peter program) into a JSON data file.
# No x86 code, PE headers or other executable content is copied.
import sys, struct, json, base64, pefile
exe, out = sys.argv[1], sys.argv[2]
pe = pefile.PE(exe)
keep = []
for s in pe.sections:
    name = s.Name.rstrip(b'\0')
    if name in (b'.rsrc', b'.petprg'):
        assert not (s.Characteristics & 0x20000020), 'section unexpectedly marked as code'
        keep.append((name, s.VirtualAddress, s.get_data()))
res_rva = pe.OPTIONAL_HEADER.DATA_DIRECTORY[2].VirtualAddress
blob = b'PETPROG1' + struct.pack('<III', pe.OPTIONAL_HEADER.SizeOfImage, res_rva, len(keep))
for name, rva, data in keep:
    blob += name.ljust(8, b'\0') + struct.pack('<II', rva, len(data)) + data
json.dump({'format': 'peter-program-data/1',
           'description': 'Resources and Peter program of Point 236 by Miroslav Nemecek (no executable code)',
           'data': base64.b64encode(blob).decode()}, open(out, 'w'))
print(out, len(blob), [k[0] for k in keep])
