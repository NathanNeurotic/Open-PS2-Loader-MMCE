from pathlib import Path
import hashlib
import re
import subprocess
import tempfile

# Compile the production functions against host I/O stubs. These checks cover
# data and launch contracts; they cannot validate PS2 drive or IOP behavior.
root = Path(__file__).resolve().parents[2]
work = tempfile.TemporaryDirectory(prefix='ra-disc-tests-')
tmp = Path(work.name)

def run(name, source, extra=()):
    path = tmp / (name + '.c')
    path.write_text(source, encoding='utf-8')
    exe = tmp / (name + '.exe')
    subprocess.run(['gcc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-I', str(root),
                    str(path), *map(str, extra), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

s = (root / 'src/ioprp.c').read_text(encoding='utf-8')
entry = s[s.index('struct romdir_entry'):s.index('/* Pointers')]
builder = s[s.index('// Physical-disc reset image:'):s.rindex('#endif')]
run('reset_image', '#include <assert.h>\n#include <string.h>\n#include <stdio.h>\n' + entry + '''
unsigned char eesync_irx[256];
unsigned int size_eesync_irx;
''' + builder + '''
int main(void) {
    unsigned char image[512];
    unsigned lengths[] = {0,1,15,16,17,255,256};
    for (unsigned n=0;n<sizeof(lengths)/sizeof(lengths[0]);n++) {
        size_eesync_irx=lengths[n];
        memset(eesync_irx, 0x61, sizeof(eesync_irx));
        memset(image, 0xcc, sizeof(image));
        unsigned size=patch_IOPRP_image_disc(image);
        struct romdir_entry *r=(void *)image;
        assert(sizeof(*r)==16);
        assert(size==112+((lengths[n]+15)&~15));
        assert(size==patch_IOPRP_image_disc_size());
        assert(!strcmp(r[0].fileName,"RESET") && r[0].fileSize==0);
        assert(!strcmp(r[1].fileName,"ROMDIR") && r[1].fileSize==80);
        assert(!strcmp(r[2].fileName,"EXTINFO") && r[2].fileSize==32);
        assert(!strcmp(r[3].fileName,"EESYNC") && r[3].fileSize==lengths[n]);
        assert(r[4].fileName[0]==0 && r[4].fileSize==0);
        assert(r[0].extinfo_size+r[3].extinfo_size==32);
        for (unsigned i=112;i<112+lengths[n];i++) assert(image[i]==0x61);
        for (unsigned i=112+lengths[n];i<size;i++) assert(image[i]==0);
        assert(image[size]==0xcc);
    }
    puts("PASS: EESYNC-only ROMDIR, payload, alignment and write bounds");
}
''')

s=(root/'src/rahash.c').read_text(encoding='utf-8')
s=re.sub(r'^#include.*$', '', s, flags=re.M)
payload=bytes(i%251 for i in range(65536+17))
expected=hashlib.md5(b'SLUS_201.74'+payload).hexdigest()
prefix=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include "include/md5.h"
typedef void (*ra_step_fn)(const char *);
typedef struct { unsigned char trycount,spindlctrl,datapattern,pad; } sceCdRMode;
enum { SCECdErNO=0,SCECdSecS2048=0,SCECdSpinNom=1,SCECdSpinStm=2 };
#define LOG(...) ((void)0)
static unsigned char iso[96*2048], payload[65536+17];
static int pos, file_mode, raw_fail, raw_calls, raw_refuse, short_reads;
static int mock_open(const char *p,int flags) { (void)p;(void)flags;pos=0;return file_mode?7:-1; }
static int mock_close(int fd) {(void)fd;return 0;}
static int mock_lseek(int fd,int off,int whence) {(void)fd;if(whence==SEEK_END)return sizeof(payload);pos=off;return off;}
static long long mock_lseek64(int fd,long long off,int whence) {(void)fd;(void)whence;pos=off;return off;}
static int mock_read(int fd,void *b,int n) {
    (void)fd;
    if(file_mode==2 && pos>0)return 0;
    if(short_reads && n>127)n=127;
    if(pos+n>(int)sizeof(payload))n=sizeof(payload)-pos;
    memcpy(b,payload+pos,n);pos+=n;return n;
}
static int sceCdDiskReady(int mode) {(void)mode;return 1;}
static int sceCdRead(unsigned lba,unsigned count,void *b,sceCdRMode *m) {
    raw_calls++;
    assert(m->trycount==32 && m->datapattern==SCECdSecS2048);
    if(raw_refuse) {raw_refuse--;return 0;}
    if(raw_fail)return 0;
    assert((lba+count)*2048<=sizeof(iso));
    memcpy(b,iso+lba*2048,count*2048);return 1;
}
static int sceCdSync(int mode) {(void)mode;return 0;}
static int sceCdGetError(void) {return 0;}
#define open mock_open
#define close mock_close
#define lseek mock_lseek
#define lseek64 mock_lseek64
#define read mock_read
'''
test=r'''
static void put32(unsigned char *p,unsigned v) {for(int i=0;i<4;i++)p[i]=v>>(8*i);}
int main(void) {
    char hash[33];
    for(unsigned i=0;i<sizeof(payload);i++)payload[i]=i%251;
    memcpy(iso+16*2048+1,"CD001",5);
    put32(iso+16*2048+158,20);put32(iso+16*2048+166,2048);
    unsigned char *r=iso+20*2048;
    r[0]=46;r[32]=12;memcpy(r+33,"SLUS_201.74;1",12);
    put32(r+2,24);put32(r+10,sizeof(payload));
    memcpy(iso+24*2048,payload,sizeof(payload));
    file_mode=1;raw_calls=0;
    assert(raHashDisc("cdrom0:\\SLUS_201.74;1","SLUS_201.74",hash)==0);
    assert(!strcmp(hash,"EXPECTED") && raw_calls==0);
    short_reads=1;
    assert(raHashDisc("disc","SLUS_201.74",hash)==0 && !strcmp(hash,"EXPECTED"));
    short_reads=0;file_mode=0;raw_calls=0;
    assert(raHashDisc("disc","SLUS_201.74",hash)==0 && !strcmp(hash,"EXPECTED"));
    assert(raw_calls==4); /* PVD, root, 64K executable, rounded sector tail */
    raw_refuse=9;
    assert(raHashDisc("disc","SLUS_201.74",hash)==0 && !strcmp(hash,"EXPECTED"));
    file_mode=2; /* partial filesystem read must recover using the full raw extent */
    assert(raHashDisc("disc","SLUS_201.74",hash)==0 && !strcmp(hash,"EXPECTED"));
    raw_fail=1;raw_calls=0;strcpy(hash,"stale");
    assert(raHashDisc("disc","SLUS_201.74",hash)<0 && hash[0]==0);
    assert(raw_calls==16);
    assert(disc_read_at(1,g_chunk,2048)<0);
    raw_fail=0;file_mode=0;put32(r+10,0);
    assert(raHashDisc("disc","SLUS_201.74",hash)<0 && hash[0]==0);
    puts("PASS: file/raw hash agreement, short reads, sector tail, retries, failure rejects partial hash");
}
'''.replace('EXPECTED', expected)
run('hash', prefix+s+test, [root/'src/md5.c'])

s=(root/'src/discsupport.c').read_text(encoding='utf-8')
s=re.sub(r'^#include.*$', '', s, flags=re.M)
labels=sorted(set(re.findall(r'_STR_\w+',s)))
prefix='''
#include <assert.h>
#include <stdio.h>
#include <string.h>
enum { '''+', '.join(labels)+''' };
#define _l(x) "notice"
enum { IO_CUSTOM_SIMPLEACTION, IO_OK, NO_EXCEPTION, IO_MODE_SELECTED_ALL };
static int gRATelemetry, image_busy, probe_result, watch_count, queued_result=IO_OK;
static int launched, torn_down, queue_count;
static const char *probe_path="cdrom0:\\\\SLUS_201.74;1", *home="mc0:/OPL";
static int sbHashGameBusy(void) {return image_busy;}
static int sysGetDiscBootPath(char *p,int n,void(*fn)(void)) {(void)fn;snprintf(p,n,"%s",probe_path);return probe_result;}
static const char *configGetHomePath(void) {return home;}
static void guiShowRANotice(const char *a,const char *b) {(void)a;(void)b;}
static void raHashLogOpen(const char *p) {(void)p;}
static void raHashStep(const char *s) {(void)s;}
static void raHashSetStepLog(void(*f)(const char*)) {(void)f;}
static void ClearWatchList(void) {}
static int raHashDisc(const char *p,const char *s,char *h) {(void)p;(void)s;h[0]=0;return -1;}
static void raHashLogAdd(const char *p,const char *s,const char *h) {(void)p;(void)s;(void)h;}
static int raAskPC(const char*h,const char*s,const char*p,char*i,int n,char*j,int m) {(void)h;(void)s;(void)p;(void)i;(void)n;(void)j;(void)m;return -1;}
static void raHashLogClose(void) {}
static int ioPutRequest(int mode,void(*fn)(void)) {(void)mode;(void)fn;queue_count++;return queued_result;}
static int sbLoadWatchList(const char *p,const char *s) {assert(!strcmp(p,"mc0:/OPL/"));assert(!strcmp(s,"SLUS_201.74"));return watch_count;}
static void mmceSendGameID(const char *s,const char *p,int mode) {assert(!strcmp(s,"SLUS_201.74"));assert(p==NULL && mode==0);}
static void deinit(int a,int b) {assert(a==NO_EXCEPTION && b==IO_MODE_SELECTED_ALL);torn_down++;}
static void sysLaunchLoaderElf(const char*s,const char*m,int n,void*p,int k,void*q,int logo,unsigned flags) {
assert(!strcmp(s,"SLUS_201.74") && !strcmp(m,"DISC_MODE"));assert(!n && !p && !k && !q && logo==1 && flags==0);assert(torn_down==1);launched++;
}
'''
test=r'''
int main(void) {
    char boot[64],startup[16],prefix[120],long_home[150];
    assert(discIdentity(boot,startup,NULL)==0 && !strcmp(startup,"SLUS_201.74"));
    probe_path="cdrom0:\\DIR\\SLUS_201.74;1";assert(discIdentity(boot,startup,NULL)<0);
    probe_path="cdrom0:\\1234567890123456;1";assert(discIdentity(boot,startup,NULL)<0);
    probe_path="mass0:/SLUS_201.74";assert(discIdentity(boot,startup,NULL)<0);
    probe_path="cdrom0:\\SLUS_201.74;1";
    assert(discSupportPrefix(prefix,sizeof(prefix))==0 && !strcmp(prefix,"mc0:/OPL/"));
    home="mc0:";assert(discSupportPrefix(prefix,sizeof(prefix))==0 && !strcmp(prefix,home));
    home="mc0:/OPL/";assert(discSupportPrefix(prefix,sizeof(prefix))==0 && !strcmp(prefix,home));
    memset(long_home,'x',sizeof(long_home)-1);long_home[sizeof(long_home)-1]=0;home=long_home;
    assert(discSupportPrefix(prefix,sizeof(prefix))<0);
    home=NULL;assert(discSupportPrefix(prefix,sizeof(prefix))<0);home="mc0:/OPL";
    image_busy=1;assert(!discCheckSupportDeferred() && queue_count==0);image_busy=0;
    queued_result=-1;assert(!discCheckSupportDeferred() && !discCheckBusy());
    queued_result=IO_OK;assert(discCheckSupportDeferred() && discCheckBusy());
    assert(!discCheckSupportDeferred());
    gRATelemetry=1;watch_count=1;discLaunch(NULL);assert(!launched && !torn_down);
    discCheckWorker();assert(!discCheckBusy());
    gRATelemetry=0;discLaunch(NULL);assert(!launched);
    gRATelemetry=1;watch_count=0;discLaunch(NULL);assert(!launched && !torn_down);
    watch_count=1;probe_result=-1;discLaunch(NULL);assert(!launched && !torn_down);
    probe_result=0;discLaunch(NULL);assert(launched==1 && torn_down==1);
    puts("PASS: disc identity, settings paths, busy/queue failure, telemetry and watch-list gates, PS2LOGO handoff");
}
'''
run('launch',prefix+s+test)

# Exercise the actual interrupt handler up to the point where it would suspend
# game threads. Hardware register reads/writes use host bytes in this harness.
s=(root/'ee_core/src/padhook.c').read_text(encoding='utf-8')
s=s[s.index('static int IGR_Intc_Handler'):]
s=s[:s.index('    // If power button or combo is press')]
s=s.replace('    int i;','    (void)cause;')
s+='    return Pad_Data.combo_type;\n}\n'
prefix=r'''
#include <assert.h>
#include <stdio.h>
#define RETROACHIEVEMENTS 1
typedef unsigned char u8;
enum { DISC_MODE=1, OTHER_MODE, IGR_LIBPAD, IGR_LIBPAD2,
       IGR_PAD_STABLE_V1, IGR_PAD_STABLE_V2, IGR_COMBO_R1_L1_R2_L2,
       IGR_COMBO_START_SELECT, IGR_COMBO_R3_L3 };
static struct { int GameMode; } cfg;
#define USE_LOCAL_EECORE_CONFIG __typeof__(cfg) *config=&cfg
#define UNCACHED_SEG(x) (x)
static struct { u8 *pad_buf; int pos_state,pos_frame,pos_combo1,pos_combo2;
 int libpad,vb_count,prev_frame,combo_type; } Pad_Data;
static struct { int press,vb_count; } Power_Button;
static u8 ndin=0x20, poff=0x04, sdin=0x55, scmd=0x55;
#define CDVD_R_NDIN (&ndin)
#define CDVD_R_POFF (&poff)
#define CDVD_R_SDIN (&sdin)
#define CDVD_R_SCMD (&scmd)
static int padOpen_hooked, snapshots, kernel_enters;
static void RA_OnVblank(void) {snapshots++;}
static void ee_kmode_enter(void) {kernel_enters++;}
static void ee_kmode_exit(void) {}
'''
test=r'''
int main(void) {
    u8 pad[]={IGR_PAD_STABLE_V1,1,IGR_COMBO_R1_L1_R2_L2,IGR_COMBO_R3_L3};
    Pad_Data.pad_buf=pad;Pad_Data.libpad=IGR_LIBPAD;
    Pad_Data.pos_state=0;Pad_Data.pos_frame=1;Pad_Data.pos_combo1=2;Pad_Data.pos_combo2=3;
    cfg.GameMode=DISC_MODE;
    assert(IGR_Intc_Handler(0)==0);
    assert(snapshots==1 && !kernel_enters && !Power_Button.press);
    assert(sdin==0x55 && scmd==0x55); /* no ROM power-off cancellation */
    pad[3]=IGR_COMBO_START_SELECT;
    assert(IGR_Intc_Handler(0)==IGR_COMBO_START_SELECT); /* return combo still reaches teardown */
    cfg.GameMode=OTHER_MODE;pad[3]=IGR_COMBO_R3_L3;
    assert(IGR_Intc_Handler(0)==IGR_COMBO_R3_L3);
    assert(sdin==0 && scmd==0x1b && Power_Button.press==1 && kernel_enters==1);
    puts("PASS: disc power-off combo cannot suspend game threads; physical button untouched; normal mode preserved");
}
'''
run('power_input',prefix+s+test)

s=(root/'src/system.c').read_text(encoding='utf-8')
s=s[s.index('static int sysParseBoot2'):s.index('// Boot the physical PS2 disc')]
run('boot_path', '#include <assert.h>\n#include <stdio.h>\n#include <string.h>\n#include <strings.h>\n'+s+r'''
int main(void) {
    char out[64], cnf[100], token[65];
    memset(token,'A',64);token[64]=0;
    snprintf(cnf,sizeof(cnf),"BOOT2 = %s",token);
    assert(sysParseBoot2(cnf,out,sizeof(out))<0 && out[0]==0);
    token[63]=0;
    const char *ends[]={"","\n","\r\n"," ","\t"};
    for(unsigned i=0;i<sizeof(ends)/sizeof(ends[0]);i++) {
        snprintf(cnf,sizeof(cnf),"BOOT2 = %s%s",token,ends[i]);
        assert(sysParseBoot2(cnf,out,sizeof(out))==0 && !strcmp(out,token));
    }
    assert(sysParseBoot2("BOOT2 = \r\n",out,sizeof(out))<0);
    assert(sysParseBoot2("BOOT2 = A",out,1)<0);
    assert(sysParseBoot2("BOOT2 = A",out,0)<0);
    assert(sysParseBoot2("boot2 = cdrom0:\\SLUS_201.74;1\r\n",out,sizeof(out))==0);
    assert(!strcmp(out,"cdrom0:\\SLUS_201.74;1"));
    puts("PASS: BOOT2 exact fit, overflow, delimiters and normal CRLF paths");
}
''')

s=(root/'src/supportbase.c').read_text(encoding='utf-8')
s=s[s.index('static char ra_hash_path'):s.index('static void sbTestPCLinkWorker')]
prefix=r'''
#include <assert.h>
#include <stdio.h>
enum { IO_OK=0, IO_CUSTOM_SIMPLEACTION=1 };
static int result, requests, hashes;
static void (*worker)(void);
static int ioPutRequest(int kind,void (*fn)(void)) {assert(kind==IO_CUSTOM_SIMPLEACTION);requests++;worker=fn;return result;}
static void sbHashGame(const char *p,const char*n,const char*e,const char*s,int f) {(void)p;(void)n;(void)e;(void)s;(void)f;hashes++;}
'''
run('image_queue',prefix+s+r'''
int main(void) {
    for(int error=-1;error>=-6;error--) {
        result=error;
        assert(!sbHashGameDeferred("p","n","iso","SLUS_201.74",1));
        assert(!sbHashGameBusy());
    }
    result=IO_OK;
    assert(sbHashGameDeferred("p","n","iso","SLUS_201.74",1));
    assert(sbHashGameBusy() && requests==7);
    assert(!sbHashGameDeferred("p","n","iso","SLUS_201.74",1) && requests==7);
    worker();assert(!sbHashGameBusy() && hashes==1);
    assert(sbHashGameDeferred("p","n","iso","SLUS_201.74",1));
    puts("PASS: failed image-hash submissions release busy state and allow a later request");
}
''')

s=(root/'ee_core/src/patches.c').read_text(encoding='utf-8')
s=s[s.index('void apply_patches(const char *path)'):]
cases=re.findall(r'case (PATCH_\w+):\s*\n\s*(?:if \(file_eq_gameid\)\s*)?(\w+)\(',s)
assert len(cases)==16, cases
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#define RETROACHIEVEMENTS 1
#define _strcmp strcmp
#define _strncmp strncmp
#define _lw(x) 0
#define _sw(v,a) ((void)0)
#define Skip_BIK_Videos() 0
#define Skip_Videos_sceMpegIsEnd() ((void)0)
enum { DISC_MODE=30,HDD_MODE,ETH_MODE,BDM_MODE,ALL_MODE,COMPAT_MODE_4=1 };
static struct { int GameMode; char GameID[16]; } cfg;
#define USE_LOCAL_EECORE_CONFIG __typeof__(cfg) *config=&cfg
static int g_compat_mask;
typedef struct { const char *game; int mode; struct { uintptr_t addr,check,val; } patch; } patchlist_t;
'''
prefix+='enum { '+', '.join(c for c,f in cases)+' };\n'
prefix+='static int hits[16];\n'
for c,f in cases:
    prefix+=f'#define {f}(...) (hits[{c}]++)\n'
prefix+='static const patchlist_t patch_list[]={\n'
for c,_ in cases:
    prefix+='{"TEST_000.00",ALL_MODE,{'+c+',0,0}},\n'
prefix+='{NULL,0,{0,0,0}}};\n'
run('patch_selection',prefix+s+r'''
int main(void) {
    strcpy(cfg.GameID,"TEST_000.00");cfg.GameMode=DISC_MODE;
    apply_patches("cdrom0:\\TEST_000.00;1");
    for(int i=0;i<16;i++) assert(hits[i]==(i==PATCH_EUTECHNYX_WU_TID || i==PATCH_PRO_SNOWBOARDER || i==PATCH_DOT_HACK));
    memset(hits,0,sizeof(hits));cfg.GameMode=BDM_MODE;
    apply_patches("cdrom0:\\TEST_000.00;1");
    for(int i=0;i<16;i++) assert(hits[i]==1);
    puts("PASS: disc thread/pad patches retained, storage patches excluded, image patch dispatch preserved");
}
''')

work.cleanup()
