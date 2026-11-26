import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class BootSerialBridgeTests(unittest.TestCase):
    def test_parser_preserves_actual_identity_and_rejects_conflicts(self):
        source = r'''
#include <stdio.h>
#include <string.h>
#include "boot_serial_bridge.h"
static int check(const char *cmd,const char *cfg,int result,const char *expected) {
 char b[256]; snprintf(b,sizeof(b),"%s",cmd);
 int rc=hmos_bridge_boot_serial(b,sizeof(b),cfg);
 if(rc!=result || strcmp(b,expected)){fprintf(stderr,"rc=%d [%s]\n",rc,b);return 1;}
 return 0;
}
int main(void) {
 int bad=0;
 bad+=check("a=1","androidboot.serialno = \"ABC123\"\n",1,"a=1 ohos.boot.sn=ABC123");
 bad+=check("x=2 androidboot.serialno=ABC123","androidboot.serialno=ABC123\n",0,"x=2 androidboot.serialno=ABC123");
 bad+=check("sn=ABC123 foo.serialno=ABC123","androidboot.serialno=ABC123\n",0,"sn=ABC123 foo.serialno=ABC123");
 bad+=check("sn=OTHER","androidboot.serialno=ABC123\n",-1,"sn=OTHER");
 bad+=check("ohos.boot.sn=/some/file","androidboot.serialno=ABC123\n",-1,"ohos.boot.sn=/some/file");
 bad+=check("a=1","androidboot.serialno=ABC123\nandroidboot.serialno=OTHER\n",-1,"a=1");
 bad+=check("a=1","androidboot.serialno=ABC123\nandroidboot.serialno=ABC123\n",1,"a=1 ohos.boot.sn=ABC123");
 bad+=check("a=1","androidboot.serialno=\"ABC 123\"\n",-1,"a=1");
 bad+=check("a=1","androidboot.serialno=\"ABC123\n",-1,"a=1");
 bad+=check("a=1","androidboot.serialno_extra=OTHER\n",0,"a=1");
 bad+=check("a=1","androidboot.serialno=\"\"\n",-1,"a=1");
 bad+=check("a=1\n", "androidboot.serialno=ABC123\r\n",1,"a=1\n ohos.boot.sn=ABC123");
 bad+=check("foo.sn=OTHER ohos.boot.serialno=ABC123", "androidboot.serialno=ABC123\n",-1,"foo.sn=OTHER ohos.boot.serialno=ABC123");
 char maximum[96]; memset(maximum,'X',95); maximum[95]=0;
 char cfg[140]; snprintf(cfg,sizeof(cfg),"androidboot.serialno=%s\n",maximum);
 char big[256]=""; bad+=hmos_bridge_boot_serial(big,sizeof(big),cfg)!=1;
 bad+=strlen(big)!=strlen(" ohos.boot.sn=")+95;
 char small[5]="a=1";
 bad+=hmos_bridge_boot_serial(small,sizeof(small),"androidboot.serialno=ABC123\n")!=-1;
 bad+=strcmp(small,"a=1")!=0;
 return !!bad;
}
'''
        with tempfile.TemporaryDirectory() as t:
            p = Path(t)
            (p/'test.c').write_text(source)
            subprocess.run(['cc','-std=c17','-O2','-Wall','-Wextra','-Werror',
                            '-I'+str(ROOT/'compat/bootstrap'),str(p/'test.c'),
                            '-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
