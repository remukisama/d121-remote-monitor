"""Verified TLS for WeChat's public endpoint and Cloud Run's local API gateway."""
from pathlib import Path
import ssl
import certifi

PLATFORM_CA=Path('/app/cert/certificate.crt')

def wechat_ssl_context(platform_ca=PLATFORM_CA):
    # Keep the system/public roots, then append only the documented platform CA.
    # This context is used exclusively by the WeChat login HTTP client.
    context=ssl.create_default_context()
    context.load_verify_locations(cafile=certifi.where())
    if platform_ca.is_file():
        context.load_verify_locations(cafile=str(platform_ca))
    return context
