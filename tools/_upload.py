import paramiko, sys

c = paramiko.SSHClient()
c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
c.connect('222.233.152.150', port=22, username='ikbeomjeon', password='jikbeom9')

sftp = c.open_sftp()
local = sys.argv[1]
remote = sys.argv[2]
sftp.put(local, remote)
sftp.close()
print(f'업로드 완료: {local} -> {remote}')
c.close()
