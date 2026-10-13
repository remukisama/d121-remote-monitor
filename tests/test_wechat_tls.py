"""Exercise real TLS with a platform CA, including hostname and trust rejection."""
from datetime import datetime,timedelta,timezone
from pathlib import Path
import socket,ssl,threading
import pytest
from cryptography import x509
from cryptography.hazmat.primitives import hashes,serialization
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.x509.oid import NameOID
from backend.wechat_tls import wechat_ssl_context

def test_platform_root_is_appended_without_disabling_certificate_checks(tmp_path):
    key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
    name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'test-platform-gateway')])
    now=datetime.now(timezone.utc)
    certificate=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
        .serial_number(x509.random_serial_number()).not_valid_before(now-timedelta(minutes=1))
        .not_valid_after(now+timedelta(days=1)).add_extension(x509.BasicConstraints(ca=True,path_length=None),critical=True)
        .add_extension(x509.SubjectAlternativeName([x509.DNSName('localhost')]),critical=False).sign(key,hashes.SHA256()))
    ca=tmp_path/'platform.crt';private=tmp_path/'server.key'
    ca.write_bytes(certificate.public_bytes(serialization.Encoding.PEM))
    private.write_bytes(key.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
    server_context=ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER);server_context.load_cert_chain(ca,private)
    listener=socket.socket();listener.bind(('127.0.0.1',0));listener.listen(3);listener.settimeout(5)
    port=listener.getsockname()[1];errors=[]
    def serve():
        try:
            for _ in range(3):
                connection,_=listener.accept();connection.settimeout(5)
                try:
                    with server_context.wrap_socket(connection,server_side=True) as stream:stream.sendall(b'verified')
                except ssl.SSLError:connection.close() # Expected client rejection in two cases.
        except Exception as error:errors.append(error)
        finally:listener.close()
    worker=threading.Thread(target=serve,daemon=True);worker.start()
    ordinary=wechat_ssl_context(tmp_path/'no-platform-ca.crt')
    trusted=wechat_ssl_context(ca)
    assert trusted.verify_mode==ssl.CERT_REQUIRED and trusted.check_hostname
    assert trusted.cert_store_stats()['x509_ca']>=ordinary.cert_store_stats()['x509_ca']+1
    try:
        with socket.create_connection(('127.0.0.1',port),timeout=5) as raw:
            with pytest.raises(ssl.SSLCertVerificationError):ordinary.wrap_socket(raw,server_hostname='localhost')
        with socket.create_connection(('127.0.0.1',port),timeout=5) as raw:
            with trusted.wrap_socket(raw,server_hostname='localhost') as stream:assert stream.recv(20)==b'verified'
        with socket.create_connection(('127.0.0.1',port),timeout=5) as raw:
            with pytest.raises(ssl.SSLCertVerificationError):trusted.wrap_socket(raw,server_hostname='wrong-host.example')
    finally:worker.join(5)
    assert not worker.is_alive() and not errors

def test_invalid_platform_certificate_fails_closed(tmp_path):
    ca=tmp_path/'platform.crt';ca.write_text('invalid certificate',encoding='utf-8')
    with pytest.raises(ssl.SSLError):wechat_ssl_context(ca)
