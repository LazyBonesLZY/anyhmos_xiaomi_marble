"""Validate Bluetooth node identities before staging; this never powers hardware."""
import re


def device_numbers(text):
    match=re.fullmatch(r'([0-9]+):([0-9]+)\s*',text)
    if not match:raise ValueError('Invalid sysfs device identity')
    major,minor=map(int,match.groups())
    if major<=0 or major>4095 or minor>1048575:
        raise ValueError('Out of range device identity')
    return major,minor


def plan(tty_identity,power_identity,existing_tty=None,existing_power=None):
    tty=device_numbers(tty_identity);power=device_numbers(power_identity)
    for expected,existing in ((tty,existing_tty),(power,existing_power)):
        if existing is not None and existing!=expected:
            raise ValueError('Existing staged device does not match live sysfs')
    return {'tty':{'path':'/dev/ttyHS0','major':tty[0],'minor':tty[1]},
            'power':{'path':'/dev/btpower','major':power[0],'minor':power[1]},
            'source':'live sysfs; never hardcode historical majors',
            'power_operation_performed':False,
            'firmware_mount_mode':'read-only',
            'hardware_initialization_verified':False}
