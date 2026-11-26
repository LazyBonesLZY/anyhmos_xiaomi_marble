"""Compose one verified stock microphone path into the shim's base XML.

No device access or mixer writes. Non-target paths/default controls remain exact.
The stock overlay replaces paths, it does not append duplicate path definitions.
"""
from copy import deepcopy
from hashlib import sha256
from pathlib import Path
import re
import xml.etree.ElementTree as ET

BASE_SHA = 'c81b983a942ce2bb353530c9d805c8f89cf2d164cee44fcdd682d8a83a921d19'
OVERLAY_SHA = 'be135b78c48221096955a26d4e9c80cd9a7c75829dcb4becccbecd13c5a9469d'
TARGET = 'handset-dmic-endfire'


def compose(base: bytes, overlay: bytes, target_name: str = TARGET) -> bytes:
    if target_name not in (TARGET, "speaker-mic"):
        raise ValueError("Unverified target microphone path")
    if sha256(base).hexdigest() != BASE_SHA or sha256(overlay).hexdigest() != OVERLAY_SHA:
        raise ValueError('Stock microphone XML identity mismatch')
    old_root, stock_root = ET.fromstring(base), ET.fromstring(overlay)
    old = old_root.findall(f"./path[@name='{target_name}']")
    stock = stock_root.findall(f"./path[@name='{target_name}']")
    if len(old) != 1 or len(stock) != 1:
        raise ValueError('Expected exactly one stock microphone path per XML')
    if target_name == TARGET:
        if any(child.tag != 'ctl' for child in stock[0]):
            raise ValueError('Unexpected nested stock path; reject unverified dependency')
        values = {ctl.attrib['name']: ctl.attrib['value'] for ctl in stock[0]}
        if values.get('TX DEC1 MUX') != 'SWR_MIC' or values.get('TX DEC3 MUX') != 'SWR_MIC':
            raise ValueError('Stock route is not the verified SoundWire microphone route')
    else:
        children = list(stock[0])
        if len(children) != 1 or children[0].tag != 'path' or children[0].attrib != {'name': 'amic5'}:
            raise ValueError('Unverified speaker microphone nested dependency')
        amic, = old_root.findall("./path[@name='amic5']")
        values = {ctl.attrib['name']: ctl.attrib['value'] for ctl in amic}
        if values.get('TX DEC3 MUX') != 'SWR_MIC' or values.get('AMIC5_MIXER Switch') != '1':
            raise ValueError('Base AMIC5 route mismatch')
    pattern = rb'<path\s+name="' + target_name.encode() + rb'"\s*>.*?</path>'
    matches = list(re.finditer(pattern, base, re.DOTALL))
    if len(matches) != 1:
        raise ValueError('Ambiguous literal base path')
    start, end = matches[0].span()
    replacement = ET.tostring(stock[0], encoding='utf-8').rstrip()
    merged = base[:start] + replacement + base[end:]
    root = ET.fromstring(merged)
    target = root.findall(f"./path[@name='{target_name}']")
    if len(target) != 1:
        raise ValueError('Composition produced duplicate microphone paths')
    expected = deepcopy(old_root)
    at = list(old_root).index(old[0])
    expected.remove(expected[at])
    expected.insert(at, deepcopy(stock[0]))
    # Whitespace around the replaced element is serialization, not control data.
    def normalized(tree):
        for node in tree.iter():
            if node.text is not None and not node.text.strip(): node.text = None
            if node.tail is not None and not node.tail.strip(): node.tail = None
        return ET.tostring(tree)
    if normalized(root) != normalized(expected):
        raise ValueError('Non-target control or path changed')
    return merged


def read_regular(path: Path) -> bytes:
    if not path.is_file() or path.is_symlink():
        raise ValueError(f'Expected regular XML input: {path}')
    return path.read_bytes()
