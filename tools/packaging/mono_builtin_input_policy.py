"""Split built-in mono PCM input from unchanged headset/Bluetooth formats."""
from copy import deepcopy
import xml.etree.ElementTree as ET


def compose(blob):
    root=ET.fromstring(blob)
    primary,=root.findall("./adapters/adapter[@name='primary']")
    pipe,=primary.findall("./pipes/pipe[@name='primary_input']")
    props,=pipe.findall('./streamProps')
    items=[x for x in props if 'Builtin_Mic_In' in [s.strip() for s in x.attrib.get('supportDevices','').split(',')]]
    if len(items)!=3:raise ValueError('Unexpected original primary input profiles')
    selected=[]
    for node in items:
        devs=[s.strip() for s in node.attrib['supportDevices'].split(',')]
        devs.remove('Builtin_Mic_In');node.set('supportDevices',', '.join(devs))
        if node.attrib.get('sampleRates')=='48000' and node.attrib.get('channelLayout')=='CH_LAYOUT_STEREO':
            selected.append(node)
    if len(selected)!=1:raise ValueError('Missing single 48k PCM primary input')
    mono=deepcopy(selected[0]);mono.set('channelLayout','CH_LAYOUT_MONO');mono.set('bufferSize','1920');mono.set('supportDevices','Builtin_Mic_In');props.append(mono)
    for other in primary.findall('./pipes/pipe'):
        if other is pipe:continue
        for node in other.findall('./streamProps/streamProp'):
            devs=[s.strip() for s in node.attrib.get('supportDevices','').split(',')]
            if 'Builtin_Mic_In' in devs:
                devs.remove('Builtin_Mic_In');node.set('supportDevices',', '.join(devs))
    mic,=primary.findall("./devices/device[@name='Builtin_Mic_In']")
    mic.set('supportPipes','primary_input')
    return ET.tostring(root,encoding='utf-8',xml_declaration=True)
