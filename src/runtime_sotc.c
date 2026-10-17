/* Shadow of the Colossus (CUSA08809) imports specific to this game.
 * Each function follows the PS4 contract as implemented by shadPS4 (GPL-2.0-or-later). */
#define _CRT_RAND_S
#include "runtime.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/syscall.h>
#endif

#define KERNEL_ERROR(e) ((int32_t)(UINT32_C(0x80020000) + (e)))
#define E_PERM 1
#define E_SRCH 3
#define E_ACCES 13
#define E_BUSY 16
#define E_INVAL 22
#define E_NAMETOOLONG 63
#define E_TIMEDOUT 60
#define E_CANCELED 85

/* libSceLibcInternalExt: libc.prx 1.01 of this game asks for the heap trace layout at start. */
typedef struct {
    uint64_t size;
    uint32_t flag, get_segment_info;
    uint64_t *mspace_atomic_id_mask, *mstate_table;
} HeapTraceInfo;
static uint64_t mspace_atomic_id_mask, mstate_table[64];
static ABI void heap_get_trace_info(HeapTraceInfo *info) {
    info->mspace_atomic_id_mask=&mspace_atomic_id_mask;
    info->mstate_table=mstate_table;
    info->get_segment_info=0;
}

/* --- libkernel: small queries --- */
/* Base PS4 by default: the game keeps its 1080p path. SOTC_NEO=1 (experimental): a PS4 Pro,
 * whose Cinematic (1440p) and Performance (60 FPS) modes the game offers in its menu. */
int runtime_sotc_neo(void) {
    static int neo=-1;
    if (neo<0) { const char *v=getenv("SOTC_NEO"); neo=v && *v=='1'; }
    return neo;
}
static ABI int32_t is_neo_mode(void) {
    static int reported;
    if (!reported++) printf("Runtime: sceKernelIsNeoMode -> %d (SOTC_NEO)\n",runtime_sotc_neo());
    return runtime_sotc_neo();
}
static ABI int32_t kernel_sleep(uint32_t seconds) {
#ifdef _WIN32
    Sleep(seconds*1000u);
#else
    sleep(seconds);
#endif
    return 0;
}
static ABI int32_t get_thread_id(void) {
#ifdef _WIN32
    return (int32_t)GetCurrentThreadId();
#else
    return (int32_t)syscall(SYS_gettid);
#endif
}
static ABI int32_t uuid_create(unsigned char *uuid) {
    if (!uuid) return KERNEL_ERROR(E_INVAL);
    for (int i=0;i<16;i+=4) {
        unsigned int r=0;
#ifdef _WIN32
        rand_s(&r);
#else
        r=(unsigned)random();
#endif
        memcpy(uuid+i,&r,4);
    }
    uuid[6]=(unsigned char)((uuid[6]&0x0f)|0x40); /* version 4 */
    uuid[8]=(unsigned char)((uuid[8]&0x3f)|0x80);
    return 0;
}

/* --- libkernel time zone: west_sec is the offset east of UTC, as libc's localtime adds it --- */
typedef struct { int64_t t; uint32_t west_sec, dst_sec; } TimeSec;
typedef struct { int32_t minutes_west, dst_time; } KernelTimezone;
static void host_zone(int32_t *offset_east, int32_t *dst) {
#ifdef _WIN32
    TIME_ZONE_INFORMATION tz;
    DWORD state=GetTimeZoneInformation(&tz);
    *offset_east=(int32_t)-tz.Bias*60;
    *dst=state==TIME_ZONE_ID_DAYLIGHT ? (int32_t)-tz.DaylightBias*60 : 0;
#else
    time_t now=time(NULL); struct tm local; localtime_r(&now,&local);
    *dst=local.tm_isdst>0 ? 3600 : 0;
    *offset_east=(int32_t)local.tm_gmtoff-*dst;
#endif
}
static ABI int32_t utc_to_local(int64_t utc, int64_t *local, TimeSec *st, uint64_t *dst_sec) {
    int32_t east, dst; host_zone(&east,&dst);
    if (local) *local=utc+east+dst;
    if (st) { st->t=utc; st->west_sec=(uint32_t)east; st->dst_sec=(uint32_t)dst; }
    if (dst_sec) *dst_sec=(uint64_t)dst;
    return 0;
}
static ABI int32_t local_to_utc(int64_t local, int64_t unused, int64_t *utc, KernelTimezone *zone, int32_t *dst_seconds) {
    (void)unused;
    if (!zone) return KERNEL_ERROR(E_INVAL);
    int32_t east, dst; host_zone(&east,&dst);
    zone->minutes_west=-east/60; zone->dst_time=dst ? 1 : 0;
    if (utc) *utc=local-east-dst;
    if (dst_seconds) *dst_seconds=dst;
    return 0;
}

/* --- libSceSysmodule: Fios2 1.01 loads an internal module by id; every library is host code here. */
static ABI int32_t load_module_internal_with_arg(uint32_t id, int32_t argc, const void *argv, uint64_t flags, int32_t *result) {
    (void)argc; (void)argv; (void)flags;
    printf("Runtime: internal system module 0x%x loaded (host implementation)\n",id);
    if (result) *result=0;
    return 0;
}

/* --- libSceDiscMap (Fios2 1.01): no disc bitmap; every request is on the HDD. --- */
#define DISC_MAP_NO_BITMAP_INFO ((int32_t)UINT32_C(0x81100004))
static ABI int32_t disc_map_no_bitmap(void) { return DISC_MAP_NO_BITMAP_INFO; }
static ABI int32_t disc_map_request(const char *path, int64_t offset, int64_t size, int32_t *flags, int32_t *a, int32_t *b) {
    (void)path; (void)offset; (void)size;
    if (flags) *flags=0;
    if (a) *a=0;
    if (b) *b=0;
    return 0;
}

/* --- System services with nothing to do on a PC: accepted and ignored. --- */
static ABI int32_t ok_ignored(void) { return 0; }
static ABI int32_t ok_args(uint64_t a, uint64_t b, uint64_t c, uint64_t d) { (void)a; (void)b; (void)c; (void)d; return 0; }
static ABI void system_load_exec(const char *path, char *const *argv) {
    (void)argv;
    fprintf(stderr,"STOP: the game asked to execute %s (sceSystemServiceLoadExec)\n",path ? path : "(null)");
    exit(0);
}
/* SceSystemServiceDisplaySafeAreaInfo: float ratio, reserved[128]; the whole window is safe. */
static ABI int32_t display_safe_area(float *info) {
    if (!info) return (int32_t)0x80A10003;
    memset(info,0,132); info[0]=1.0f;
    return 0;
}
/* libSceSystemGesture: no touch pad events (the pad runtime reports no touches). */
static int gesture_handle=0;
static ABI int32_t gesture_open(int32_t input, const void *param) { (void)input; (void)param; return ++gesture_handle; }
static ABI int32_t gesture_events_count(int32_t handle, const void *recognizer) { (void)handle; (void)recognizer; return 0; }
static ABI int32_t gesture_event_by_index(int32_t handle, const void *recognizer, uint32_t index, void *event) {
    (void)handle; (void)recognizer; (void)index; (void)event;
    return (int32_t)0x808AD003; /* index out of range */
}
/* libScePngEnc: screenshots for save icons are not encoded; the game keeps its default icon. */
#define PNG_ENC_ERROR ((int32_t)UINT32_C(0x80690004))
static ABI int32_t png_query_memory(const void *param) { (void)param; return 4096; }
static ABI int32_t png_create(const void *param, void *memory, uint32_t size, void **handle) {
    (void)param; (void)size;
    if (!memory || !handle) return PNG_ENC_ERROR;
    *handle=memory;
    return 0;
}
static ABI int32_t png_encode(void *handle, const void *param, void *output) { (void)handle; (void)param; (void)output; return PNG_ENC_ERROR; }
/* libSceHttp helpers used without a connection. */
static ABI int32_t http_uri_escape(char *out, uint64_t *require, uint64_t prepare, const char *in) {
    static const char hex[]="0123456789ABCDEF";
    if (!in) return (int32_t)0x80431002;
    uint64_t n=0;
    for (const unsigned char *p=(const unsigned char *)in;*p;++p) {
        int plain=(*p>='A' && *p<='Z') || (*p>='a' && *p<='z') || (*p>='0' && *p<='9') || strchr("-_.~",*p);
        if (out && n+(plain ? 1 : 3)<prepare) {
            if (plain) out[n]=(char)*p;
            else { out[n]='%'; out[n+1]=hex[*p>>4]; out[n+2]=hex[*p&15]; }
        }
        n+=plain ? 1 : 3;
    }
    if (out && prepare) out[n<prepare ? n : prepare-1]=0;
    if (require) *require=n+1;
    return out && n+1>prepare ? (int32_t)0x80431100 : 0;
}
static ABI int32_t net_unavailable(void) { return (int32_t)0x80410101; }

/* --- Silent audio (temporary): libSceNgs2 renders silence, libSceAudio3d accepts objects and
 * paces the audio thread. Real sound will come from the user's own firmware modules. --- */
typedef struct { uint32_t magic, kind; uint64_t rack_id; } AudioHandle;
static AudioHandle *audio_handle(uint32_t kind, uint64_t rack_id) {
    AudioHandle *h=calloc(1,sizeof(*h));
    if (h) { h->magic=0x4e475332; h->kind=kind; h->rack_id=rack_id; }
    return h;
}
#define NGS2_ERROR_INVALID ((int32_t)UINT32_C(0x804a0230))
static ABI int32_t ngs2_system_create(const void *option, const void *allocator, void **out) {
    (void)option; (void)allocator;
    if (!out) return NGS2_ERROR_INVALID;
    *out=audio_handle(1,0);
    puts("Runtime: Ngs2 system created (silent audio)");
    return 0;
}
typedef struct { void *buffer; uint64_t size; uint32_t waveform, channels; } Ngs2RenderBuffer;
static ABI int32_t ngs2_system_render(void *system, Ngs2RenderBuffer *buffers, uint32_t count) {
    (void)system;
    for (uint32_t i=0;buffers && i<count;++i) if (buffers[i].buffer) memset(buffers[i].buffer,0,buffers[i].size);
    return 0;
}
typedef struct { void *host_buffer; uint64_t host_buffer_size; uintptr_t reserved[5], user_data; } Ngs2ContextBuffer;
static ABI int32_t ngs2_rack_query_buffer_size(uint32_t rack_id, const void *option, Ngs2ContextBuffer *info) {
    (void)rack_id; (void)option;
    if (!info) return NGS2_ERROR_INVALID;
    info->host_buffer_size=0x1000;
    return 0;
}
static ABI int32_t ngs2_rack_create(void *system, uint32_t rack_id, const void *option, const void *buffer, void **out) {
    (void)system; (void)option; (void)buffer;
    if (!out) return NGS2_ERROR_INVALID;
    *out=audio_handle(2,rack_id);
    return 0;
}
static ABI int32_t ngs2_rack_voice(void *rack, uint32_t index, void **out) {
    (void)rack; (void)index;
    if (!out) return NGS2_ERROR_INVALID;
    *out=audio_handle(3,index);
    return 0;
}
static ABI int32_t ngs2_voice_state(void *voice, void *state, uint64_t size) {
    (void)voice;
    if (state && size) memset(state,0,size);
    return 0;
}
static ABI int32_t ngs2_voice_flags(void *voice, uint32_t *flags) { (void)voice; if (flags) *flags=0; return 0; }
static ABI int32_t audio3d_port_open(int32_t user, const void *params, uint32_t *port) {
    (void)user; (void)params;
    if (!port) return (int32_t)0x80ec0004;
    *port=1;
    puts("Runtime: Audio3d port opened (silent audio)");
    return 0;
}
static ABI int32_t audio3d_object_reserve(uint32_t port, uint32_t *object) {
    static _Atomic uint32_t next=1;
    (void)port;
    if (!object) return (int32_t)0x80ec0004;
    *object=next++;
    return 0;
}
static ABI int32_t audio3d_port_push(uint32_t port, int blocking) {
    (void)port;
    if (blocking) { /* one 256-sample grain at 48 kHz */
#ifdef _WIN32
        Sleep(5);
#else
        usleep(5333);
#endif
    }
    return 0;
}
static ABI int32_t audio3d_queue_level(uint32_t port, uint32_t *level, uint32_t *available) {
    (void)port;
    if (level) *level=0;
    if (available) *available=4;
    return 0;
}

/* --- POSIX scheduling: FreeBSD policies, PS4 FIFO priorities 256..767 --- */
enum { SCHED_FIFO_=1, SCHED_OTHER_=2, SCHED_RR_=3 };
static ABI int32_t sched_priority_max(int policy) { return policy==SCHED_FIFO_ || policy==SCHED_RR_ ? 767 : E_INVAL; }
static ABI int32_t sched_priority_min(int policy) { return policy==SCHED_FIFO_ || policy==SCHED_RR_ ? 256 : E_INVAL; }
static ABI int32_t get_sched_param(void *thread, int *policy, int *param) {
    (void)thread;
    if (!policy || !param) return E_INVAL;
    *policy=SCHED_OTHER_; *param=700;
    return 0;
}
static ABI int32_t set_sched_param(void *thread, int policy, const int *param) {
    (void)thread; (void)policy;
    return param ? 0 : E_INVAL; /* host scheduling stays with Windows */
}
static ABI int32_t mutexattr_set_protocol(void *attr, int protocol) {
    return attr && protocol>=0 && protocol<=2 ? 0 : E_INVAL; /* priority protocols do not apply on the host */
}

/* --- libkernel event flags --- */
enum { EVF_AND=1, EVF_OR=2, EVF_CLEAR_ALL=0x10, EVF_CLEAR_PAT=0x20 };
typedef struct EventWaiter {
    pthread_cond_t cv;
    uint64_t bits, result;
    int mode, ready, canceled, deleted;
    struct EventWaiter *next;
} EventWaiter;
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t gone;
    uint64_t bits;
    int multi, deleted, traced;
    EventWaiter *waiters;
    char name[32];
} EventFlag;
static int evf_met(const EventFlag *f, uint64_t bits, int mode) {
    return (mode&0xf)==EVF_AND ? (f->bits&bits)==bits : (f->bits&bits)!=0;
}
static void evf_clear(EventFlag *f, uint64_t bits, int mode) {
    if (mode&EVF_CLEAR_ALL) f->bits=0;
    else if (mode&EVF_CLEAR_PAT) f->bits&=~bits;
}
static void evf_unlink(EventFlag *f, EventWaiter *w) {
    for (EventWaiter **p=&f->waiters;*p;p=&(*p)->next) if (*p==w) { *p=w->next; break; }
    if (f->deleted && !f->waiters) pthread_cond_broadcast(&f->gone);
}
static int evf_valid_mode(int mode) {
    int wait=mode&0xf, clear=mode&0xf0;
    return (wait==EVF_AND || wait==EVF_OR) && (clear==0 || clear==EVF_CLEAR_ALL || clear==EVF_CLEAR_PAT);
}
static ABI int32_t evf_create(EventFlag **out, const char *name, uint32_t attr, uint64_t pattern, const void *option) {
    if (!out || !name || option || attr>0x22) return KERNEL_ERROR(E_INVAL);
    if (strlen(name)>=32) return KERNEL_ERROR(E_NAMETOOLONG);
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return KERNEL_ERROR(12);
    pthread_mutex_init(&f->lock,NULL); pthread_cond_init(&f->gone,NULL);
    f->bits=pattern; f->multi=(attr&0xf0)==0x20;
    strcpy(f->name,name);
    static const char *trace; static int trace_checked;
    if (!trace_checked) { trace=getenv("SOTC_EVF_TRACE"); trace_checked=1; }
    f->traced=trace && (!strcmp(trace,"*") || strstr(trace,name)!=NULL);
    if (f->traced) fprintf(stderr,"EVF create '%s' %p attr 0x%x bits 0x%llx from %p\n",name,(void *)f,attr,
                           (unsigned long long)pattern,__builtin_return_address(0));
    *out=f;
    return 0;
}
static ABI int32_t evf_delete(EventFlag *f) {
    if (!f) return KERNEL_ERROR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    f->deleted=1;
    for (EventWaiter *w=f->waiters;w;w=w->next) { w->deleted=1; pthread_cond_signal(&w->cv); }
    while (f->waiters) pthread_cond_wait(&f->gone,&f->lock);
    pthread_mutex_unlock(&f->lock);
    /* Freed late on purpose: a racing Set/Poll after delete must not touch freed memory. */
    return 0;
}
static ABI int32_t evf_set(EventFlag *f, uint64_t bits) {
    if (!f) return KERNEL_ERROR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    if (f->traced) fprintf(stderr,"EVF set %p 0x%llx (was 0x%llx) from %p thread %lu\n",(void *)f,(unsigned long long)bits,
                           (unsigned long long)f->bits,__builtin_return_address(0),(unsigned long)get_thread_id());
    f->bits|=bits;
    for (EventWaiter *w=f->waiters;w;w=w->next) {
        if (w->ready || !evf_met(f,w->bits,w->mode)) continue;
        w->result=f->bits; evf_clear(f,w->bits,w->mode);
        w->ready=1; pthread_cond_signal(&w->cv);
    }
    pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t evf_clear_bits(EventFlag *f, uint64_t bits) {
    if (!f) return KERNEL_ERROR(E_SRCH);
    pthread_mutex_lock(&f->lock); f->bits&=bits; pthread_mutex_unlock(&f->lock);
    return 0;
}
static ABI int32_t evf_cancel(EventFlag *f, uint64_t pattern, int *count) {
    if (!f) return KERNEL_ERROR(E_SRCH);
    pthread_mutex_lock(&f->lock);
    int n=0;
    f->bits=pattern;
    for (EventWaiter *w=f->waiters;w;w=w->next) { ++n; w->result=pattern; w->canceled=1; pthread_cond_signal(&w->cv); }
    if (count) *count=n;
    pthread_mutex_unlock(&f->lock);
    return 0;
}
/* timeout: NULL waits forever; *timeout==0 is a poll (EBUSY when not met). */
static int32_t evf_wait_common(EventFlag *f, uint64_t bits, int mode, uint64_t *result, uint32_t *timeout, int poll) {
    if (!f) return KERNEL_ERROR(E_SRCH);
    if (!bits || !evf_valid_mode(mode)) return KERNEL_ERROR(E_INVAL);
    pthread_mutex_lock(&f->lock);
    if (f->deleted) { pthread_mutex_unlock(&f->lock); return KERNEL_ERROR(E_ACCES); }
    if (!f->multi && f->waiters) { pthread_mutex_unlock(&f->lock); return KERNEL_ERROR(E_PERM); }
    if (evf_met(f,bits,mode)) {
        if (result) *result=f->bits;
        evf_clear(f,bits,mode);
        pthread_mutex_unlock(&f->lock);
        return 0;
    }
    if (poll) {
        if (result) *result=f->bits;
        pthread_mutex_unlock(&f->lock);
        return KERNEL_ERROR(E_BUSY);
    }
    EventWaiter w={.bits=bits,.mode=mode};
    pthread_cond_init(&w.cv,NULL);
    EventWaiter **tail=&f->waiters;
    while (*tail) tail=&(*tail)->next;
    *tail=&w;
    struct timespec deadline;
    if (timeout) {
        clock_gettime(CLOCK_REALTIME,&deadline);
        uint64_t ns=(uint64_t)deadline.tv_nsec+(uint64_t)*timeout*1000u;
        deadline.tv_sec+=(time_t)(ns/1000000000u); deadline.tv_nsec=(long)(ns%1000000000u);
    }
    int timed_out=0, reported=0;
    while (!w.ready && !w.canceled && !w.deleted) {
        if (!timeout) {
            /* Infinite waits wake every 5 s to report long waits (hang diagnosis). */
            struct timespec slice;
            clock_gettime(CLOCK_REALTIME,&slice); slice.tv_sec+=5;
            if (pthread_cond_timedwait(&w.cv,&f->lock,&slice)==ETIMEDOUT && !w.ready && !w.canceled && !w.deleted && reported<3) {
                ++reported;
                fprintf(stderr,"Runtime: event flag '%s' waited %ds for 0x%llx (mode 0x%x), bits 0x%llx\n",f->name,
                        reported*5,(unsigned long long)bits,mode,(unsigned long long)f->bits);
            }
        }
        else if (pthread_cond_timedwait(&w.cv,&f->lock,&deadline)==ETIMEDOUT && !w.ready && !w.canceled && !w.deleted) {
            timed_out=1; break;
        }
    }
    evf_unlink(f,&w);
    if (result) *result=timed_out ? f->bits : w.result;
    pthread_mutex_unlock(&f->lock);
    pthread_cond_destroy(&w.cv);
    if (timed_out) { *timeout=0; return KERNEL_ERROR(E_TIMEDOUT); }
    if (w.canceled) return KERNEL_ERROR(E_CANCELED);
    if (w.deleted) return KERNEL_ERROR(E_ACCES);
    return 0;
}
static ABI int32_t evf_wait(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result, uint32_t *timeout) {
    if (f && f->traced) fprintf(stderr,"EVF wait %p 0x%llx mode 0x%x (bits 0x%llx) from %p thread %lu\n",(void *)f,
                                (unsigned long long)bits,mode,(unsigned long long)f->bits,__builtin_return_address(0),
                                (unsigned long)get_thread_id());
    int32_t r=evf_wait_common(f,bits,(int)mode,result,timeout,0);
    if (f && f->traced) fprintf(stderr,"EVF woke %p -> 0x%x thread %lu\n",(void *)f,(unsigned)r,(unsigned long)get_thread_id());
    return r;
}
static ABI int32_t evf_poll(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    return evf_wait_common(f,bits,(int)mode,result,NULL,1);
}

static ABI int32_t ulobjmgr_register(uint64_t object, int32_t kind, uint32_t *id) {
    if (!object || !kind || !id) return 22; /* EINVAL */
    *id=0;
    return 0;
}
static ABI void guest_sincosf(float x, float *sine, float *cosine) { *sine=sinf(x); *cosine=cosf(x); }
static ABI int32_t ulobjmgr_unregister(uint32_t id) { return id>=0x4000 ? 22 : 0; }
static ABI int32_t ulobjmgr_check(uint64_t object) { return object ? 0 : 22; }
static const RuntimeExport exports[]={
    {"sceLibcHeapGetTraceInfo",heap_get_trace_info},
    {"sceKernelIsNeoMode",is_neo_mode}, {"sceKernelSleep",kernel_sleep},
    {"scePthreadGetthreadid",get_thread_id}, {"sceKernelUuidCreate",uuid_create},
    {"sceSysmoduleLoadModuleInternalWithArg",load_module_internal_with_arg},
    {"sceShareUtilityInitializeEx2",ok_args}, {"sceShareUtilityOpenShareMenuDefault",ok_ignored},
    {"sceScreenShotDisable",ok_ignored}, {"sceScreenShotEnable",ok_ignored},
    {"sceGameLiveStreamingEnableLiveStreaming",ok_args}, {"sceVideoRecordingSetInfo",ok_args},
    {"sceNetCtlInit",ok_ignored}, {"sceNetCtlTerm",ok_ignored},
    {"sceNpTrophyDestroyContext",ok_args}, {"sceNpTrophyDestroyHandle",ok_args},
    {"scePadSetLightBar",ok_args}, {"sceAudioOutSetMixLevelPadSpk",ok_args},
    {"sceImeKeyboardOpen",ok_args}, {"sceImeUpdate",ok_args}, {"sceImeKeyboardClose",ok_args},
    {"sceSystemServiceLoadExec",system_load_exec}, {"sceSystemServiceGetDisplaySafeAreaInfo",display_safe_area},
    {"sceSystemGestureOpen",gesture_open}, {"sceSystemGestureClose",ok_args},
    {"sceSystemGestureCreateTouchRecognizer",ok_args}, {"sceSystemGestureUpdateTouchRecognizer",ok_args},
    {"sceSystemGestureInitializePrimitiveTouchRecognizer",ok_args},
    {"sceSystemGestureFinalizePrimitiveTouchRecognizer",ok_args},
    {"sceSystemGestureUpdatePrimitiveTouchRecognizer",ok_args},
    {"sceSystemGestureGetTouchEventsCount",gesture_events_count},
    {"sceSystemGestureGetTouchEventByIndex",gesture_event_by_index},
    {"scePngEncQueryMemorySize",png_query_memory}, {"scePngEncCreate",png_create},
    {"scePngEncDelete",ok_args}, {"scePngEncEncode",png_encode},
    {"sceHttpUriEscape",http_uri_escape}, {"sceHttpGetAllResponseHeaders",net_unavailable},
    {"sceNetGetSockInfo",net_unavailable},
    {"sceNgs2SystemCreateWithAllocator",ngs2_system_create}, {"sceNgs2SystemDestroy",ok_args},
    {"sceNgs2SystemRender",ngs2_system_render}, {"sceNgs2RackQueryBufferSize",ngs2_rack_query_buffer_size},
    {"sceNgs2RackCreate",ngs2_rack_create}, {"sceNgs2RackCreateWithAllocator",ngs2_rack_create},
    {"sceNgs2RackDestroy",ok_args}, {"sceNgs2RackGetVoiceHandle",ngs2_rack_voice},
    {"sceNgs2VoiceControl",ok_args}, {"sceNgs2VoiceGetState",ngs2_voice_state},
    {"sceNgs2VoiceGetStateFlags",ngs2_voice_flags},
    {"sceAudio3dInitialize",ok_args}, {"sceAudio3dTerminate",ok_ignored},
    {"sceAudio3dPortOpen",audio3d_port_open}, {"sceAudio3dPortClose",ok_args},
    {"sceAudio3dObjectReserve",audio3d_object_reserve}, {"sceAudio3dObjectUnreserve",ok_args},
    {"sceAudio3dObjectSetAttributes",ok_args}, {"sceAudio3dPortSetAttribute",ok_args},
    {"sceAudio3dPortAdvance",ok_args}, {"sceAudio3dPortPush",audio3d_port_push},
    {"sceAudio3dPortGetQueueLevel",audio3d_queue_level},
    {"sceKernelConvertUtcToLocaltime",utc_to_local}, {"sceKernelConvertLocaltimeToUtc",local_to_utc},
    {"sched_get_priority_max",sched_priority_max}, {"sched_get_priority_min",sched_priority_min},
    {"pthread_getschedparam",get_sched_param}, {"pthread_setschedparam",set_sched_param},
    {"pthread_mutexattr_setprotocol",mutexattr_set_protocol},
    {"sceKernelCreateEventFlag",evf_create}, {"sceKernelDeleteEventFlag",evf_delete},
    {"sceKernelSetEventFlag",evf_set}, {"sceKernelClearEventFlag",evf_clear_bits},
    {"sceKernelCancelEventFlag",evf_cancel}, {"sceKernelWaitEventFlag",evf_wait},
    {"sceKernelPollEventFlag",evf_poll},
};
/* Imports without a public symbol name, matched by NID. */
static const struct { const char *nid; void *function; } by_nid[]={
    {"fl1eoDnwQ4s",disc_map_no_bitmap}, /* sceDiscMapGetPackageSize */
    {"lbQKqsERhtE",disc_map_no_bitmap}, /* sceDiscMapIsRequestOnHDD */
    {"fJgP+wqifno",disc_map_request},
    {"ioKMruft1ek",disc_map_no_bitmap},
    {"5+vOlukvkfg",disc_map_no_bitmap},
    /* libkernel ulobjmgr (libSceUlt's object registry), as in shadPS4. */
    {"BG26hBGiNlw",ulobjmgr_register}, {"Smf+fUNblPc",ulobjmgr_unregister},
    {"HZ9Q2c+4BU4",ok_ignored}, {"SweJO7t3pkk",ulobjmgr_check},
    {"pztV4AF18iI",guest_sincosf}, /* sincosf (libSceLibcInternal, libSceNgs2) */
};
uintptr_t runtime_sotc_resolve(const char *name) {
    for (size_t i=0;i<sizeof(by_nid)/sizeof(*by_nid);++i)
        if (!strcmp(name,by_nid[i].nid)) return (uintptr_t)by_nid[i].function;
    return RUNTIME_LOOKUP(exports,name);
}
