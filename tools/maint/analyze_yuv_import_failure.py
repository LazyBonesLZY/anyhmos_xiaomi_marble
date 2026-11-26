"""Bind native import failures to adjacent actual buffer metadata, not decoder counters."""
import re


def summarize(text):
    latest=None
    failures=[]
    for line in text.splitlines():
        if 'HMOS_IMPORT_V4 ' in line:
            latest={k:v for k,v in re.findall(r'(\w+)=([^ ]+)',line)}
        elif 'HMOS_HANDLE_V4 ' in line and latest:
            h={k:v for k,v in re.findall(r'(\w+)=([^ ]+)',line)}
            if h.get('format') not in ('24','25','28','29'):continue
            if latest.get('valid')=='0':
                failures.append({'format':int(h['format']),'width':int(h['width']),
                                 'height':int(h['height']),'stride':int(h['stride']),
                                 'fd_valid':h.get('fstat')=='0','backend_valid':False})
            latest=None
    return {'failed_yuv_imports':failures,
            'vk_create_image_failure_observed':'vkCreateImage failed' in text,
            'decoder_failure_proven':False,
            'scope':'Adjacent runtime metadata; no pixels copied or format guessed.'}
