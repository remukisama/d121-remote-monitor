import json
import os
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import termios

def crc(data):
    value=0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value=(value>>1)^0xa001 if value&1 else value>>1
    return value

def pack(address,cmd,payload=b''):
    body=bytes((address,cmd,len(payload)))+payload
    return b'\xaa\x55'+body+struct.pack('<H',crc(body))+b'\r\n'

class Master:
    def __init__(self,fd):self.fd=fd;self.buffer=bytearray()
    def receive(self,timeout=1):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if len(self.buffer)>=5:
                assert self.buffer[:2]==b'\xaa\x55',self.buffer
                size=self.buffer[4]+9
                if len(self.buffer)>=size:
                    raw=bytes(self.buffer[:size]);del self.buffer[:size]
                    assert raw[-2:]==b'\r\n'
                    assert crc(raw[2:-4])==struct.unpack('<H',raw[-4:-2])[0]
                    return raw[2],raw[3],raw[5:-4]
            ready,_,_=select.select([self.fd],[],[],max(0,deadline-time.monotonic()))
            if ready:self.buffer.extend(os.read(self.fd,4096))
        return None

with tempfile.TemporaryDirectory() as directory:
    master_fd,slave_fd=os.openpty();uart=os.ttyname(slave_fd)
    original_termios=termios.tcgetattr(slave_fd)
    notify=socket.socket(socket.AF_UNIX,socket.SOCK_DGRAM)
    name='\0loongson-test-'+str(os.getpid());notify.bind(name);notify.settimeout(2)
    env=os.environ.copy();env['NOTIFY_SOCKET']='@'+name[1:]
    status=directory+'/status.json'
    process=subprocess.Popen(['build/host-slave','--legacy','--test-uart',uart,'--sample-ms','200','--stats',status],
                             stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,env=env)
    try:
        assert process.stdout.readline().startswith('START ')
        assert b'READY=1' in notify.recv(1024)
        master=Master(master_fd);time.sleep(.5)
        os.write(master_fd,pack(2,4)+pack(2,5))
        address,cmd,payload=master.receive();assert (address,cmd,len(payload))==(2,0x84,64)
        assert payload[0:4]==bytes((1,2,15,2))
        assert struct.unpack_from('<I',payload,4)[0]>=2
        assert struct.unpack_from('<I',payload,8)[0]>0
        assert struct.unpack_from('<I',payload,12)[0]<=600
        assert struct.unpack_from('<H',payload,16)[0]<=10000
        total,available=struct.unpack_from('<II',payload,20);assert 0<=available<=total and total>0
        address,cmd,payload=master.receive();assert (address,cmd,len(payload))==(2,0x85,32)
        assert payload[8:24]==b'LS2K0300-LoongOS'
        os.write(master_fd,pack(2,1));address,cmd,payload=master.receive()
        assert (address,cmd,len(payload))==(2,0x81,24) and payload[22:]==b'\0\xa1'
        echo=bytes(range(64));os.write(master_fd,pack(2,3,echo));assert master.receive()==(2,0x83,echo)
        for request in [pack(255,4),pack(1,4),pack(2,0x84),pack(2,2,b'\1'),pack(2,4,b'x')]:
            os.write(master_fd,request);assert master.receive(.12) is None
        bad=bytearray(pack(2,4));bad[-4]^=0x80
        os.write(master_fd,bad+pack(2,4));assert master.receive()[1]==0x84
        os.write(master_fd,pack(2,4)[:4]);time.sleep(.2)
        os.write(master_fd,b'\xaa'+pack(2,4));assert master.receive()[1]==0x84
        request=pack(2,5)
        for part in (request[:1],request[1:3],request[3:]):os.write(master_fd,part);time.sleep(.01)
        assert master.receive()[1]==0x85
        time.sleep(1.1)
        with open(status) as stream:local=json.load(stream)
        assert local['valid']==15 and local['sequence']>=5
        assert local['crc_errors']>=1 and local['partial_timeouts']>=1 and local['rejected']>=2
        assert local['tx_errors']==0 and local['responses']>=7
        assert b'WATCHDOG=1' in notify.recv(1024)
        process.send_signal(signal.SIGTERM)
        output=process.communicate(timeout=2)[0];assert process.returncode==0,output
        assert 'STOP exit=0' in output
        assert termios.tcgetattr(slave_fd)==original_termios
        print('INTEGRATION_PASS actual executable: 64-byte system/identity/legacy/echo, invalid/broadcast/foreign frames, fragmented streams, status contents, abstract systemd notify, graceful stop and termios restore (PTY; hardware GPIO skipped)')
    finally:
        if process.poll() is None:process.kill();process.wait()
        os.close(master_fd);os.close(slave_fd);notify.close()
for args in [('--address','1'),('--address','255'),('--sample-ms','0'),('--duration','-1'),('--garbage',)]:
    result=subprocess.run(['build/host-slave',*args],capture_output=True)
    assert result.returncode==2,args
print('CLI_GUARDS_PASS')
