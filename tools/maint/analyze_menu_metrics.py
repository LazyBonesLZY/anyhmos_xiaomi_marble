"""Summarize measured menu batches without equating idle gaps with UI stalls."""
import re
from decimal import Decimal


def summarize(text):
    begin=re.search(r'MEASURE_BEGIN uptime=([0-9.]+)',text)
    end=re.search(r'MEASURE_END uptime=([0-9.]+)',text)
    if not begin or not end:
        raise ValueError('Missing measured action boundaries')
    start_ns=int(Decimal(begin[1])*1_000_000_000)
    end_ns=int(Decimal(end[1])*1_000_000_000)
    if end_ns<=start_ns:
        raise ValueError('Invalid action interval')
    batches=[]
    for line in text.splitlines():
        if 'MEASURE_BATCH ' not in line or ' submit frames=' not in line:continue
        fields=dict(re.findall(r'(\w+)=([0-9]+)',line))
        required=('t_ns','frames','span_ms','max_us','over25ms')
        if not all(k in fields for k in required):raise ValueError('Incomplete submit batch')
        f={k:int(v) for k,v in fields.items()}
        lo=f['t_ns']-f['span_ms']*1_000_000
        batches.append({'frames':f['frames'],'max_service_us':f['max_us'],
                        'service_over25ms':f['over25ms'],
                        'batch_end_ns':f['t_ns'],'batch_start_estimate_ns':lo,
                        'overlaps_action':f['t_ns']>=start_ns and lo<=end_ns,
                        'fully_within_action':lo>=start_ns and f['t_ns']<=end_ns})
    pacing=[]
    for line in text.splitlines():
        if 'MEASURE_BATCH ' not in line or ' SUBMIT_PACING ' not in line:continue
        f={k:int(v) for k,v in re.findall(r'(\w+)=([0-9]+)',line)}
        pacing.append({'entry_max_us':f.get('entry_max_us'),
                       'contains_pre_action_or_idle_gap':f.get('entry_max_us',0)>(end_ns-start_ns)//1000,
                       'fence_error':f.get('fence_error'), 'clock_errors':f.get('clock_errors')})
    return {'action_interval_ns':[start_ns,end_ns],'batches':batches,'pacing':pacing,
            'page_latency_measured':False,
            'limitations':['Counters aggregate frames, including carried previous-entry intervals.',
                           'Action interval includes helper waits; not first-paint or touch latency.',
                           'No claim of menu latency improvement from these counters alone.']}
