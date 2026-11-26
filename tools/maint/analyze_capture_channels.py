"""Summarize real probe channel statistics, never synthesize PCM or quality claims."""
import math
import re


def summarize(text):
    channels=[]
    for line in text.splitlines():
        if not line.startswith('MIC_CHANNEL '):continue
        d={k:int(v) for k,v in re.findall(r'(\w+)=(-?\d+)',line)}
        n=d['pairs']
        if n<=0:raise ValueError('No channel samples')
        mean=d['sum']/n
        mean_square=d['squares']/n
        channels.append({'id':d['id'],'samples':n,'nonzero':d['nonzero'],
                         'peak_s16':d['peak'],'dc_s16':mean,
                         'rms_s16':math.sqrt(mean_square),
                         'ac_rms_s16':math.sqrt(max(0,mean_square-mean*mean))})
    if len(channels)!=2:raise ValueError('Expected two captured channel statistics')
    pair=re.search(r'MIC_PAIR equal=(\d+) pairs=(\d+) cross=(-?\d+)',text)
    if not pair or int(pair[2])<=0:raise ValueError('Missing paired samples')
    equal,n,cross=map(int,pair.groups())
    if equal>n:raise ValueError('Invalid equal pair count')
    return {'channels':channels,'identical_pair_ratio':equal/n,
            'two_independent_microphones_proven':False,
            'identical_channels_observed':equal==n,
            'quality_or_source_correlation_proven':False,
            'cross_product_sum':cross}
