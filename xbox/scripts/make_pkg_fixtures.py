"""Generate encrypted PKG/PFSC test fixtures with newly generated, non-console RSA keys."""
from pathlib import Path
import json
import struct
import sys
import zlib
import hashlib
import hmac
from cryptography.hazmat.primitives.asymmetric import rsa, padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
derived_rsa = rsa.generate_private_key(public_exponent=65537, key_size=2048)
fake_rsa = rsa.generate_private_key(public_exponent=65537, key_size=2048)
keysets = {}
for name, key in [('PkgDerivedKey3Keyset', derived_rsa), ('FakeKeyset', fake_rsa)]:
    k = key.private_numbers()
    keysets[name] = {n: v.to_bytes(s, 'big').hex() for n, v, s in [
        ('PublicExponent', k.public_numbers.e, 4), ('Modulus', k.public_numbers.n, 256),
        ('Prime1', k.p, 128), ('Prime2', k.q, 128), ('Exponent1', k.dmp1, 128),
        ('Exponent2', k.dmq1, 128), ('Coefficient', k.iqmp, 128), ('PrivateExponent', k.d, 256)]}
(out / 'keys.json').write_text(json.dumps(keysets, indent=2))
with (out / 'keys.txt').open('w') as f:
    for name in keysets:
        for field, value in keysets[name].items(): f.write(f'{name} {field} {value}\n')

def pkg(kind):
    blocks = [bytearray(65536) for _ in range(8)]
    # PFS mode bit 2 marks the image as XTS-encrypted; bit 3 matches the
    # case-insensitive layout used by the reference extractor.
    struct.pack_into('<H', blocks[0], 0x1c, 0x000c)
    struct.pack_into('<I', blocks[0], 0x20, 65536)
    struct.pack_into('<Q', blocks[0], 0x30, 6)
    struct.pack_into('<Q', blocks[0], 0x48, 1)
    payload = b'\x7fELF' + b'Xbox lab synthetic executable; do not execute.'
    data = bytes(range(256)) * 256 + b'END'
    for i, mode, size, count, loc in [(1,0x4000,65536,1,2),(2,0x4000,65536,1,3),
            (3,0x8000,len(payload),1,4),(4,0x4000,65536,1,5),(5,0x8000,len(data),2,6)]:
        at = i * 0xa8
        struct.pack_into('<H', blocks[1], at, mode)
        struct.pack_into('<Q', blocks[1], at+8, size)
        struct.pack_into('<II', blocks[1], at+96, count, loc)
    def directory(block, entries):
        blocks[block] = bytearray(65536)
        offset = 0
        for inode, typ, name in entries:
            raw = name.encode(); length = (16 + len(raw) + 3) & ~3
            struct.pack_into('<IIII', blocks[block], offset, inode, typ, len(raw), length)
            blocks[block][offset+16:offset+16+len(raw)] = raw
            offset += length
    directory(2, [(2,3,'uroot')])
    directory(3, [(3,2,'../outside' if kind=='traversal' else 'eboot.bin'), (4,3,'assets')])
    directory(5, [(5,2,'payload.dat')])
    if kind == 'cycle': directory(5, [(2,3,'loop')])
    if kind == 'bad_dirent': struct.pack_into('<I', blocks[3], 12, 1)
    if kind == 'bad_inode': struct.pack_into('<I', blocks[1], 3*0xa8+100, 0xffffffff)
    blocks[4][:len(payload)] = payload
    blocks[6][:65536] = data[:65536]; blocks[7][:3] = data[65536:]
    compressed = [zlib.compress(b) if i != 7 else bytes(b) for i,b in enumerate(blocks)]
    if kind == 'bad_zlib': compressed[4] = b'bad zlib'
    pfsc = bytearray(4096)
    offsets = [4096]
    for b in compressed: offsets.append(offsets[-1] + len(b))
    struct.pack_into('<IIIIQQQQ', pfsc, 0, 0x43534650, 0, 0, 65536, 65536, 48, 4096, 8*65536)
    for i,v in enumerate(offsets): struct.pack_into('<Q', pfsc, 48+i*8, v)
    if kind == 'bad_map': struct.pack_into('<Q', pfsc, 56, 0)
    pfsc += b''.join(compressed)
    plain = bytearray(0x20000) + pfsc
    plain += bytes((-len(plain)) % 4096)
    ekpfs = bytes(range(32)); dk3 = bytes(range(32,64)); seed = bytes(range(16))
    xtskeys = hmac.new(ekpfs, b'\x01\0\0\0'+seed, hashlib.sha256).digest()
    encrypted = bytearray(plain)
    for offset in range(0x10000,len(plain),4096):
        cipher = Cipher(algorithms.AES(xtskeys[16:]+xtskeys[:16]), modes.XTS((offset//4096).to_bytes(16,'little'))).encryptor()
        encrypted[offset:offset+4096] = cipher.update(plain[offset:offset+4096]) + cipher.finalize()
    encrypted[0x370:0x380] = seed
    header = bytearray(0x1000)
    struct.pack_into('>IIIIIII', header, 0, 0x7f434e54, 1, 0, 3, 3, 0, 0x1000)
    header[0x40:0x64] = b'XX0000-XBOX00001_00-SYNTHETICFIXTURE00'[:36]
    assert len(header) == 4096
    pfs_offset = 0x4000
    struct.pack_into('>QQ', header, 0x410, pfs_offset, len(encrypted))
    struct.pack_into('>Q', header, 0x430, pfs_offset + len(encrypted))
    entrykeys = bytearray(32+7*32+7*256)
    entrykeys[32+7*32+3*256:32+7*32+4*256] = derived_rsa.public_key().encrypt(dk3,padding.PKCS1v15())
    sfo = b'\0PSF'+bytes(16)
    entries = [struct.pack('>IIIIIIQ',0x10,0,0,0,0x1100,len(entrykeys),0),
               struct.pack('>IIIIIIQ',0x20,0,0,0,0x2000,256,0),
               struct.pack('>IIIIIIQ',0x1000,0,0,0,0x2100,len(sfo),0)]
    ivkey = hashlib.sha256(entries[1]+dk3).digest()
    imagekey = fake_rsa.public_key().encrypt(ekpfs,padding.PKCS1v15())
    cipher = Cipher(algorithms.AES(ivkey[16:]),modes.CBC(ivkey[:16])).encryptor()
    imagekey = cipher.update(imagekey)+cipher.finalize()
    result = header + bytearray(pfs_offset-4096)
    result[0x1000:0x1060]=b''.join(entries)
    result[0x1100:0x1100+len(entrykeys)] = entrykeys
    result[0x2000:0x2100] = imagekey; result[0x2100:0x2100+len(sfo)] = sfo
    result += encrypted
    if kind=='bad_table': struct.pack_into('>I', result, 0x10, 0xffffffff)
    if kind=='bad_rsa': result[0x2000:0x2100] = bytes(256)
    if kind=='truncated': result = result[:4200]
    return result, payload, data

for kind in ['valid','traversal','cycle','bad_dirent','bad_inode','bad_zlib','bad_map','bad_table','bad_rsa','truncated']:
    result, payload, data = pkg(kind)
    (out / f'{kind}.pkg').write_bytes(result)
(out / 'expected-eboot.bin').write_bytes(payload)
(out / 'expected-payload.dat').write_bytes(data)
