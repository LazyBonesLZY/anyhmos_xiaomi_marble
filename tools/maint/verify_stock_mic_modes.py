"""Verify stock capture route semantics without modifying routing or hardware."""
import xml.etree.ElementTree as ET


def describe(resource_xml, overlay_xml):
    root=ET.fromstring(resource_xml)
    devices=[d for d in root.iter('in-device') if d.findtext('id')=='PAL_DEVICE_IN_SPEAKER_MIC']
    if len(devices)!=1:raise ValueError('Ambiguous stock speaker input')
    d=devices[0]
    low=[u for u in d.findall('usecase') if u.findtext('name')=='PAL_STREAM_LOW_LATENCY']
    voip=[u for u in d.findall('usecase') if u.findtext('name')=='PAL_STREAM_VOIP_TX']
    if len(low)!=1 or len(voip)!=1:raise ValueError('Missing verified usecases')
    if low[0].find('channels') is not None or low[0].find('snd_device_name') is not None:
        raise ValueError('Unexpected low latency override')
    if d.findtext('channels')!='1' or d.findtext('snd_device_name')!='speaker-mic':
        raise ValueError('Unexpected default speaker microphone mode')
    if voip[0].findtext('channels')!='2' or voip[0].findtext('snd_device_name')!='speaker-dmic-endfire':
        raise ValueError('Unexpected voice microphone route')
    paths=ET.fromstring(overlay_xml)
    mono,=paths.findall("./path[@name='speaker-mic']")
    stereo,=paths.findall("./path[@name='speaker-dmic-endfire']")
    if len(mono)!=1 or mono[0].attrib!={'name':'amic5'}:raise ValueError('Unexpected mono path')
    controls={c.attrib['name']:c.attrib['value'] for c in stereo}
    for key,value in {'TX DEC1 MUX':'SWR_MIC','TX DEC3 MUX':'SWR_MIC',
                      'AMIC1_MIXER Switch':'1','AMIC5_MIXER Switch':'1'}.items():
        if controls.get(key)!=value:raise ValueError('Unexpected stereo microphone controls')
    return {'low_latency':{'hardware_channels':1,'path':'speaker-mic','source':'AMIC5'},
            'voip_tx':{'hardware_channels':2,'path':'speaker-dmic-endfire','sources':['AMIC1','AMIC5']},
            'stereo_low_latency_is_not_stock':True,
            'action':'Test truthful mono LOW_LATENCY capture before advertising independent stereo.',
            'no_voice_usecase_substitution':True}
