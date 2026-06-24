import paramiko, sys

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect('222.233.152.150', port=22, username='ikbeomjeon', password='jikbeom9')

cmd = sys.argv[1] if len(sys.argv) > 1 else 'echo hello'
_, out, err = c.exec_command(cmd)
stdout = out.read().decode('cp949', errors='replace')
stderr = err.read().decode('cp949', errors='replace')
if stdout: sys.stdout.buffer.write(b'OUT: ' + stdout.encode('utf-8', errors='replace') + b'\n')
if stderr: sys.stdout.buffer.write(b'ERR: ' + stderr.encode('utf-8', errors='replace') + b'\n')
c.close()
