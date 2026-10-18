import subprocess, sys
start=int(sys.argv[1],16); n=int(sys.argv[2],16)
img=open('halo_flat.bin','rb').read()
chunk=img[start-0x10000:start-0x10000+n]
open('/tmp/claude_chunk.bin','wb').write(chunk)
out=subprocess.run(['ndisasm','-b','32','-o',hex(start),'/tmp/claude_chunk.bin'],capture_output=True,text=True).stdout
print('\n'.join(l[:110] for l in out.splitlines()))
