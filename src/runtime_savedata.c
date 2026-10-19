/* libSceSaveData on host directories:
 *   <user>/savedata/<user id>/<title id>/<dir name>/        files the game writes
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_sys/ param.bin, icon0.png
 *   <user>/savedata/<user id>/<title id>/<dir name>.sce_backup/{data,meta}/  last good copy
 *   <user>/savedata/<user id>/<title id>.memory/memory.dat  SaveDataMemory
 * A mounted directory appears to the guest as /savedata0../savedata15.
 * Metadata lives beside the directory so the guest's own files are untouched. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#ifndef _WIN32
#include <ftw.h>
#else
__declspec(dllimport) int __stdcall MoveFileExA(const char *from, const char *to, unsigned long flags);
#endif

#define ERR_PARAMETER ((int32_t)0x809F0000)
#define ERR_NOT_INITIALIZED ((int32_t)0x809F0001)
#define ERR_BUSY ((int32_t)0x809F0003)
#define ERR_NOT_MOUNTED ((int32_t)0x809F0004)
#define ERR_EXISTS ((int32_t)0x809F0007)
#define ERR_NOT_FOUND ((int32_t)0x809F0008)
#define ERR_INTERNAL ((int32_t)0x809F000B)
#define ERR_MOUNT_FULL ((int32_t)0x809F000C)
#define ERR_BAD_MOUNTED ((int32_t)0x809F000D)
#define ERR_INVALID_USER ((int32_t)0x809F0011)
#define ERR_MEMORY_NOT_READY ((int32_t)0x809F0012)
#define MODE_RDONLY 1
#define MODE_CREATE 4
#define MODE_COPY_ICON 16
#define MODE_CREATE2 32
#define SLOTS 16

typedef struct { char data[10]; char pad[6]; } TitleId;
typedef struct { char data[32]; } DirName;
typedef struct { char data[16]; } MountPoint;
typedef struct {
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param; int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
} Param;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; const void *fingerprint;
                 uint64_t blocks; uint32_t mode; uint8_t reserved[32]; } Mount1;
typedef struct { int32_t user; int32_t pad; const DirName *dir; uint64_t blocks; uint32_t mode;
                 uint8_t reserved[32]; int32_t pad2; } Mount2;
typedef struct { MountPoint point; uint64_t required_blocks; uint32_t unused, status; uint8_t reserved[28]; int32_t pad; } MountResult;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t unused;
                 uint8_t reserved[32]; int32_t pad2; } Delete;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; uint32_t key, order;
                 uint8_t reserved[32]; } SearchCond;
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SearchInfo;
typedef struct { uint32_t hits; int32_t pad; DirName *names; uint32_t names_capacity, set_count;
                 Param *params; SearchInfo *infos; uint8_t reserved[12]; int32_t pad2; } SearchResult;
typedef struct { const void *buffer; uint64_t buffer_size, data_size; uint8_t reserved[32]; } Icon;
_Static_assert(sizeof(Param)==1328,"OrbisSaveDataParam layout");
_Static_assert(sizeof(Mount1)==80 && sizeof(Mount2)==64,"OrbisSaveDataMount layouts");
_Static_assert(sizeof(MountResult)==64,"OrbisSaveDataMountResult layout");
_Static_assert(sizeof(SearchCond)==64 && sizeof(SearchResult)==56,"dir name search layouts");

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int initialized;
static char title_id[16]="UNKNOWN";
static struct { int used, writable; uint64_t blocks; char host[700], meta[700]; } slots[SLOTS];
static char memory_path[720];
static uint64_t memory_size;
static size_t mounts_done, memory_writes;

static void repair_all_meta(void);
void runtime_savedata_configure(const char *title) {
    if (title && *title) snprintf(title_id,sizeof(title_id),"%s",title);
    repair_all_meta();
}

static int make_dirs(const char *path) {
    char buffer[700];
    snprintf(buffer,sizeof(buffer),"%s",path);
    for (char *p=buffer+1;*p;++p) if (*p=='/') { *p=0; mkdir(buffer,0755); *p='/'; }
    return mkdir(buffer,0755) && errno!=EEXIST ? -1 : 0;
}
static void root(int32_t user, const char *title, char *out, size_t size) {
    snprintf(out,size,"%s/savedata/%d/%s",runtime_file_user_dir(),user,title && *title ? title : title_id);
}
static int valid_name(const char *name, size_t max) {
    size_t n=strnlen(name,max);
    if (!n || n==max) return 0;
    for (size_t i=0;i<n;++i) if (name[i]=='/' || name[i]=='\\' || (name[i]=='.' && (i==0 || name[i-1]=='.'))) return 0;
    return 1;
}
/* sotc: replaces a file with a fully written temporary, so a crash or a closed window never
 * leaves it truncated. 0 on success. */
static int replace_file(const char *tmp, const char *path) {
#ifdef _WIN32
    if (MoveFileExA(tmp,path,1|8)) return 0; /* REPLACE_EXISTING | WRITE_THROUGH */
#else
    if (!rename(tmp,path)) return 0;
#endif
    remove(tmp);
    return -1;
}
static int write_param(const char *meta, const Param *p) {
    char path[700], tmp[710];
    snprintf(path,sizeof(path),"%s/param.bin",meta);
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    FILE *f=fopen(tmp,"wb");
    Param copy=*p; copy.mtime=time(NULL);
    const int ok=f && fwrite(&copy,sizeof(copy),1,f)==1;
    if (f && fclose(f)) { remove(tmp); f=NULL; }
    if (!ok || !f || replace_file(tmp,path)) {
        if (f) remove(tmp);
        fprintf(stderr,"Runtime: cannot write save metadata %s (errno %d)\n",path,errno);
        return -1;
    }
    return 0;
}
static int read_param(const char *meta, Param *p) {
    char path[700]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    memset(p,0,sizeof(*p));
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    size_t n=fread(p,sizeof(*p),1,f); fclose(f);
    struct stat st;
    if (!stat(path,&st)) p->mtime=st.st_mtime;
    return n==1 ? 0 : -1;
}
/* Sizes in 32 KiB blocks, as the system reports them: a save's quota is the block count it
 * was created with (at least 96, the system minimum), kept in <dir>.sce_sys/blocks. Reporting
 * a large quota for every save makes Shadow of the Colossus add them up and refuse to save
 * ("no space"). */
#define BLOCK_SIZE 32768ull
#define BLOCKS_MIN 96ull
static uint64_t used_bytes(const char *path) {
    uint64_t total=0;
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char child[1100]; snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        struct stat st;
        if (stat(child,&st)) continue;
        total+=S_ISDIR(st.st_mode) ? used_bytes(child) : (uint64_t)st.st_size;
    }
    if (d) closedir(d);
    return total;
}
static uint64_t used_blocks(const char *host) { return (used_bytes(host)+BLOCK_SIZE-1)/BLOCK_SIZE; }
static uint64_t quota_blocks(const char *meta) {
    char path[720]; snprintf(path,sizeof(path),"%s/blocks",meta);
    unsigned long long blocks=0;
    FILE *f=fopen(path,"r");
    if (f) { if (fscanf(f,"%llu",&blocks)!=1) blocks=0; fclose(f); }
    return blocks>BLOCKS_MIN ? blocks : BLOCKS_MIN;
}
static int write_quota(const char *meta, uint64_t blocks) {
    char path[720]; snprintf(path,sizeof(path),"%s/blocks",meta);
    FILE *f=fopen(path,"w");
    if (f && fprintf(f,"%llu\n",(unsigned long long)blocks)>0 && !fclose(f)) return 0;
    if (f) fclose(f);
    fprintf(stderr,"Runtime: cannot write save quota %s (errno %d)\n",path,errno);
    return -1;
}
/* sotc: saves copied from shadPS4 (or by hand) keep their metadata in <dir>/sce_sys/param.sfo
 * and have no <dir>.sce_sys beside them. Without it SaveIcon failed (ERR_INTERNAL), SetParam
 * wrote nothing, the slot list hid the save and newest-first ordering saw date 0. The sidecar
 * is created from param.sfo when there is one (zeroed otherwise), dated like the save itself. */
static uint32_t le32(const uint8_t *p) { return p[0]|p[1]<<8|p[2]<<16|(uint32_t)p[3]<<24; }
static int sfo_param(const char *path, Param *p) {
    FILE *f=fopen(path,"rb");
    if (!f) return -1;
    static uint8_t buf[65536]; /* under `lock` or at startup */
    const size_t n=fread(buf,1,sizeof(buf),f);
    fclose(f);
    if (n<20 || memcmp(buf,"\0PSF",4)) return -1;
    const uint32_t keys=le32(buf+8), data=le32(buf+12), count=le32(buf+16);
    for (uint32_t i=0;i<count && 20+16*(size_t)(i+1)<=n;++i) {
        const uint8_t *e=buf+20+16*i;
        const size_t key=(size_t)keys+(e[0]|e[1]<<8), len=le32(e+4), off=(size_t)data+le32(e+12);
        if (key>=n || off>n || len>n-off || !memchr(buf+key,0,n-key)) continue;
        const char *name=(const char *)buf+key, *v=(const char *)buf+off;
        const int vlen=(int)strnlen(v,len);
        if (!strcmp(name,"MAINTITLE")) snprintf(p->title,sizeof(p->title),"%.*s",vlen,v);
        else if (!strcmp(name,"SUBTITLE")) snprintf(p->subtitle,sizeof(p->subtitle),"%.*s",vlen,v);
        else if (!strcmp(name,"DETAIL")) snprintf(p->detail,sizeof(p->detail),"%.*s",vlen,v);
        else if (!strcmp(name,"SAVEDATA_LIST_PARAM") && len>=4) p->user_param=le32(buf+off);
    }
    return 0;
}
static time_t newest_mtime(const char *path) {
    time_t newest=0;
    DIR *d=opendir(path);
    for (struct dirent *e; d && (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..") || !strcmp(e->d_name,"sce_sys")) continue;
        char child[1100]; snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        struct stat st;
        if (stat(child,&st)) continue;
        const time_t t=S_ISDIR(st.st_mode) ? newest_mtime(child) : st.st_mtime;
        if (t>newest) newest=t;
    }
    if (d) closedir(d);
    return newest;
}
static void copy_file(const char *from, const char *to) {
    FILE *in=fopen(from,"rb");
    if (!in) return;
    FILE *out=fopen(to,"wb");
    char buf[16384];
    for (size_t n; out && (n=fread(buf,1,sizeof(buf),in));) if (fwrite(buf,1,n,out)!=n) break;
    if (out) fclose(out);
    fclose(in);
}
static int ensure_meta(const char *host, const char *meta, const char *name) {
    char path[720]; snprintf(path,sizeof(path),"%s/param.bin",meta);
    struct stat st;
    if (!stat(path,&st)) return 0;
    if (make_dirs(meta)) { fprintf(stderr,"Runtime: cannot create %s (errno %d)\n",meta,errno); return -1; }
    Param p; memset(&p,0,sizeof(p));
    char sys[720]; snprintf(sys,sizeof(sys),"%s/sce_sys/param.sfo",host);
    const int from_sfo=!sfo_param(sys,&p);
    if (write_param(meta,&p)) return -1;
    char icon[720], icon_from[720];
    snprintf(icon,sizeof(icon),"%s/icon0.png",meta);
    snprintf(icon_from,sizeof(icon_from),"%s/sce_sys/icon0.png",host);
    if (stat(icon,&st)) copy_file(icon_from,icon);
    const time_t when=newest_mtime(host);
    if (when) { struct utimbuf u={when,when}; utime(path,&u); }
    printf("Runtime: save data '%s': metadata created%s\n",name,from_sfo ? " from sce_sys/param.sfo" : "");
    return 0;
}
/* At startup: every save of this title, of every user, gets its sidecar. */
static void repair_all_meta(void) {
    char users[700];
    snprintf(users,sizeof(users),"%s/savedata",runtime_file_user_dir());
    DIR *ud=opendir(users);
    for (struct dirent *u; ud && (u=readdir(ud));) {
        if (u->d_name[0]=='.') continue;
        char base[1100]; snprintf(base,sizeof(base),"%s/%s/%s",users,u->d_name,title_id);
        DIR *d=opendir(base);
        for (struct dirent *e; d && (e=readdir(d));) {
            const size_t n=strlen(e->d_name);
            if (e->d_name[0]=='.' || n>=32 || (n>8 && !strcmp(e->d_name+n-8,".sce_sys"))) continue;
            char host[1200], meta[1200]; struct stat st;
            snprintf(host,sizeof(host),"%s/%s",base,e->d_name);
            snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
            if (strlen(meta)<680 && !stat(host,&st) && S_ISDIR(st.st_mode)) ensure_meta(host,meta,e->d_name);
        }
        if (d) closedir(d);
    }
    if (ud) closedir(ud);
}
static void space(const char *host, const char *meta, uint64_t *blocks, uint64_t *free_blocks) {
    const uint64_t quota=quota_blocks(meta), used=used_blocks(host);
    *blocks=quota; *free_blocks=quota>used ? quota-used : 0;
}
#ifdef _WIN32
static void remove_tree(const char *path) { compat_remove_tree(path); }
#else
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw; return remove(path);
}
static void remove_tree(const char *path) { nftw(path,remove_entry,16,FTW_DEPTH|FTW_PHYS); }
#endif

static ABI int32_t save_initialize(const void *param) { (void)param; initialized=1; return 0; }
static ABI int32_t save_terminate(void) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    for (int i=0;i<SLOTS;++i) if (slots[i].used) {
        char point[32]; snprintf(point,sizeof(point),"/savedata%d",i);
        runtime_file_unmount(point); slots[i].used=0;
    }
    pthread_mutex_unlock(&lock);
    initialized=0; return 0;
}
static int32_t mount(int32_t user, const char *title, const DirName *dir, uint64_t blocks, uint32_t mode, MountResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (user<0) return ERR_INVALID_USER;
    if (!dir || !result || !valid_name(dir->data,sizeof(dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(user,title,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,dir->data);
    struct stat st;
    int exists=!stat(host,&st) && S_ISDIR(st.st_mode);
    if ((mode & MODE_CREATE) && exists) return ERR_EXISTS;
    if (!(mode & (MODE_CREATE|MODE_CREATE2)) && !exists) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int slot=-1;
    for (int i=0;i<SLOTS;++i) {
        if (slots[i].used && !strcmp(slots[i].host,host)) { pthread_mutex_unlock(&lock); return ERR_BUSY; }
        if (!slots[i].used && slot<0) slot=i;
    }
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_MOUNT_FULL; }
    if (!exists && (make_dirs(host) || make_dirs(meta))) {
        pthread_mutex_unlock(&lock);
        fprintf(stderr,"Runtime: cannot create save data %s (errno %d)\n",host,errno);
        return ERR_INTERNAL;
    }
    if (!exists) { Param empty={0}; write_param(meta,&empty); }
    else ensure_meta(host,meta,dir->data);
    if (!exists || blocks>quota_blocks(meta)) write_quota(meta,blocks>BLOCKS_MIN ? blocks : BLOCKS_MIN);
    slots[slot].used=1;
    slots[slot].writable=!(mode & MODE_RDONLY);
    snprintf(slots[slot].host,sizeof(slots[slot].host),"%s",host);
    snprintf(slots[slot].meta,sizeof(slots[slot].meta),"%s",meta);
    memset(result,0,sizeof(*result));
    char point[32];
    snprintf(point,sizeof(point),"/savedata%d",slot&15);
    memcpy(result->point.data,point,strlen(point)+1);
    result->status=exists ? 0 : 1; /* CREATED */
    runtime_file_mount(result->point.data,host);
    ++mounts_done;
    pthread_mutex_unlock(&lock);
    printf("Runtime: save data '%s' mounted at %s (%s%s)\n",dir->data,result->point.data,
           exists ? "existing" : "created",(mode & MODE_RDONLY) ? ", read-only" : "");
    return 0;
}
static ABI int32_t save_mount(const Mount1 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    return mount(m->user,m->title ? m->title->data : NULL,m->dir,m->blocks,m->mode,result);
}
static ABI int32_t save_mount2(const Mount2 *m, MountResult *result) {
    if (!m) return ERR_PARAMETER;
    const int32_t r=mount(m->user,NULL,m->dir,m->blocks,m->mode,result);
    if (r || getenv("SOTC_SAVE_TRACE"))
        printf("Runtime: sceSaveDataMount2(user=%d, '%.32s', blocks=%llu, mode=0x%x) -> 0x%x\n",m->user,
               m->dir ? m->dir->data : "",(unsigned long long)m->blocks,m->mode,(unsigned)r);
    return r;
}
static int slot_of(const MountPoint *point) {
    if (!point || strncmp(point->data,"/savedata",9)) return -1;
    int slot=atoi(point->data+9);
    return slot>=0 && slot<SLOTS && slots[slot].used ? slot : -1;
}
/* sotc: backups (SotC unmounts every save with UmountWithBackup and asks for the backup when a
 * save is broken). A save unmounted after writing is copied to <dir>.sce_backup: a crash or a
 * power cut during the next write leaves that copy to restore. Copy to .tmp, then swap. */
static int copy_tree(const char *from, const char *to) {
    if (make_dirs(to)) return -1;
    int result=0;
    DIR *d=opendir(from);
    if (!d) return -1;
    for (struct dirent *e; (e=readdir(d));) {
        if (!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        char a[1100], b[1100]; struct stat st;
        snprintf(a,sizeof(a),"%s/%s",from,e->d_name);
        snprintf(b,sizeof(b),"%s/%s",to,e->d_name);
        if (stat(a,&st)) { result=-1; continue; }
        if (S_ISDIR(st.st_mode)) { if (copy_tree(a,b)) result=-1; continue; }
        FILE *in=fopen(a,"rb"), *out=in ? fopen(b,"wb") : NULL;
        char buf[16384]; size_t n=0;
        while (out && (n=fread(buf,1,sizeof(buf),in))) if (fwrite(buf,1,n,out)!=n) { result=-1; break; }
        if (!in || !out || ferror(in)) result=-1;
        if (out && fclose(out)) result=-1;
        if (in) fclose(in);
    }
    closedir(d);
    return result;
}
static int move_dir(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from,to,8) ? 0 : -1; /* WRITE_THROUGH */
#else
    return rename(from,to);
#endif
}
static void backup_paths(const char *host, char *backup, size_t size) { snprintf(backup,size,"%s.sce_backup",host); }
static int make_backup(const char *host, const char *meta) {
    char backup[720], tmp[740], part[760];
    backup_paths(host,backup,sizeof(backup));
    snprintf(tmp,sizeof(tmp),"%s.tmp",backup);
    remove_tree(tmp);
    snprintf(part,sizeof(part),"%s/data",tmp);
    int r=copy_tree(host,part);
    snprintf(part,sizeof(part),"%s/meta",tmp);
    if (!r) r=copy_tree(meta,part);
    if (!r) { remove_tree(backup); r=move_dir(tmp,backup); }
    if (r) { remove_tree(tmp); fprintf(stderr,"Runtime: cannot back up save data %s (errno %d)\n",host,errno); }
    return r;
}
static ABI int32_t save_umount(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot>=0) { runtime_file_unmount(point->data); slots[slot].used=0; }
    pthread_mutex_unlock(&lock);
    if (slot<0 || getenv("SOTC_SAVE_TRACE"))
        printf("Runtime: sceSaveDataUmount(%.16s) -> 0x%x\n",point ? point->data : "",slot>=0 ? 0 : (unsigned)ERR_NOT_FOUND);
    return slot>=0 ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_set_param(const MountPoint *point, uint32_t type, const void *buffer, uint64_t size) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    Param p; read_param(slots[slot].meta,&p);
    switch (type) {
    case 0: if (size<sizeof(Param)) goto bad; memcpy(&p,buffer,sizeof(p)); break;     /* ALL */
    case 1: snprintf(p.title,sizeof(p.title),"%.*s",(int)size,(const char *)buffer); break;
    case 2: snprintf(p.subtitle,sizeof(p.subtitle),"%.*s",(int)size,(const char *)buffer); break;
    case 3: snprintf(p.detail,sizeof(p.detail),"%.*s",(int)size,(const char *)buffer); break;
    case 4: if (size<4) goto bad; memcpy(&p.user_param,buffer,4); break;
    default: goto bad;
    }
    const int32_t r=write_param(slots[slot].meta,&p) ? ERR_INTERNAL : 0;
    pthread_mutex_unlock(&lock);
    return r;
bad:
    pthread_mutex_unlock(&lock);
    return ERR_PARAMETER;
}
static ABI int32_t save_icon(const MountPoint *point, const Icon *icon) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!icon || !icon->buffer) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    int slot=slot_of(point);
    int32_t r=ERR_NOT_MOUNTED;
    if (slot>=0) {
        char path[700], tmp[710];
        snprintf(path,sizeof(path),"%s/icon0.png",slots[slot].meta);
        snprintf(tmp,sizeof(tmp),"%s.tmp",path);
        make_dirs(slots[slot].meta);
        FILE *f=fopen(tmp,"wb");
        r=f && fwrite(icon->buffer,1,icon->data_size,f)==icon->data_size ? 0 : ERR_INTERNAL;
        if (f && fclose(f)) r=ERR_INTERNAL;
        if (!r && replace_file(tmp,path)) r=ERR_INTERNAL;
        if (r) { remove(tmp); fprintf(stderr,"Runtime: cannot write save icon %s (errno %d)\n",path,errno); }
    }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t save_delete(const Delete *d) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!d || !d->dir || !valid_name(d->dir->data,sizeof(d->dir->data))) return ERR_PARAMETER;
    char base[600], host[640], meta[680];
    root(d->user,d->title ? d->title->data : NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,d->dir->data);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,d->dir->data);
    struct stat st;
    if (stat(host,&st)) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int mounted=0;
    for (int i=0;i<SLOTS;++i) if (slots[i].used && !strcmp(slots[i].host,host)) mounted=1;
    pthread_mutex_unlock(&lock);
    if (mounted) return ERR_BUSY;
    remove_tree(host);
    remove_tree(meta);
    char backup[720]; backup_paths(host,backup,sizeof(backup)); remove_tree(backup);
    if (!stat(host,&st)) { fprintf(stderr,"Runtime: cannot delete save data %s\n",host); return ERR_INTERNAL; }
    printf("Runtime: save data '%s' deleted\n",d->dir->data);
    return 0;
}
/* SQL-LIKE pattern: % any run, _ one character. */
static int like(const char *s, const char *p) {
    if (!*p) return !*s;
    if (*p=='%') { for (;;++s) { if (like(s,p+1)) return 1; if (!*s) return 0; } }
    return *s && (*p=='_' || *p==*s) && like(s+1,p+1);
}
typedef struct { char name[32]; Param param; uint64_t blocks, free_blocks; } Entry;
static uint32_t sort_key, sort_order;
static int compare(const void *a, const void *b) {
    const Entry *x=a, *y=b;
    int c = sort_key==1 ? (x->param.user_param>y->param.user_param)-(x->param.user_param<y->param.user_param)
          : sort_key==3 ? (x->param.mtime>y->param.mtime)-(x->param.mtime<y->param.mtime)
          : strcmp(x->name,y->name);
    return sort_order ? -c : c;
}
static ABI int32_t save_search(const SearchCond *cond, SearchResult *result) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!cond || !result) return ERR_PARAMETER;
    char base[600];
    root(cond->user,cond->title ? cond->title->data : NULL,base,sizeof(base));
    Entry *entries=NULL; size_t count=0, capacity=0;
    DIR *d=opendir(base);
    for (struct dirent *e; d && (e=readdir(d));) {
        size_t n=strlen(e->d_name);
        if (e->d_name[0]=='.' || n>=32 || (n>8 && !strcmp(e->d_name+n-8,".sce_sys")) || strstr(e->d_name,".sce_backup") ||
            strstr(e->d_name,".restore")) continue;
        if (cond->dir && cond->dir->data[0] && !like(e->d_name,cond->dir->data)) continue;
        char host[680]; snprintf(host,sizeof(host),"%s/%s",base,e->d_name);
        struct stat st;
        if (stat(host,&st) || !S_ISDIR(st.st_mode)) continue; /* save data are directories */
        if (count==capacity) { capacity=capacity ? capacity*2 : 16; entries=realloc(entries,capacity*sizeof(*entries)); }
        snprintf(entries[count].name,32,"%s",e->d_name);
        char meta[680]; snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,e->d_name);
        read_param(meta,&entries[count].param);
        space(host,meta,&entries[count].blocks,&entries[count].free_blocks);
        ++count;
    }
    if (d) closedir(d);
    sort_key=cond->key; sort_order=cond->order;
    if (count) qsort(entries,count,sizeof(*entries),compare);
    result->hits=(uint32_t)count;
    uint32_t set=count<result->names_capacity ? (uint32_t)count : result->names_capacity;
    for (uint32_t i=0;i<set;++i) {
        if (result->names) memcpy(result->names[i].data,entries[i].name,32);
        if (result->params) result->params[i]=entries[i].param;
        if (result->infos) {
            memset(&result->infos[i],0,sizeof(SearchInfo));
            result->infos[i].blocks=entries[i].blocks; result->infos[i].free_blocks=entries[i].free_blocks;
        }
    }
    result->set_count=set;
    free(entries);
    if (getenv("SOTC_SAVE_TRACE"))
        printf("Runtime: sceSaveDataDirNameSearch('%.32s', key=%u) -> %u hits\n",cond->dir ? cond->dir->data : "",
               cond->key,result->hits);
    return 0;
}
/* SaveDataMemory: one fixed-size blob per user/title. */
static ABI int32_t memory_setup(int32_t user, uint64_t size, const Param *param) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!size) return ERR_PARAMETER;
    char base[600], dir[640];
    root(user,NULL,base,sizeof(base));
    snprintf(dir,sizeof(dir),"%s.memory",base);
    if (make_dirs(dir)) return ERR_INTERNAL;
    pthread_mutex_lock(&lock);
    snprintf(memory_path,sizeof(memory_path),"%s/memory.dat",dir);
    FILE *f=fopen(memory_path,"r+b");
    if (!f) f=fopen(memory_path,"w+b");
    int32_t r=ERR_INTERNAL;
    if (f) {
        fseek(f,0,SEEK_END);
        if ((uint64_t)ftell(f)<size && ftruncate(fileno(f),(off_t)size)) goto done;
        memory_size=size; r=0;
    done:
        fclose(f);
    }
    if (!r && param) write_param(dir,param);
    pthread_mutex_unlock(&lock);
    if (!r) printf("Runtime: save data memory ready (%llu bytes)\n",(unsigned long long)size);
    return r;
}
static int32_t memory_io(void *buffer, uint64_t size, int64_t offset, int write) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    if (!buffer || offset<0) return ERR_PARAMETER;
    pthread_mutex_lock(&lock);
    if (!memory_size) { pthread_mutex_unlock(&lock); return ERR_MEMORY_NOT_READY; }
    if ((uint64_t)offset+size>memory_size) { pthread_mutex_unlock(&lock); return ERR_PARAMETER; }
    FILE *f=fopen(memory_path,"r+b");
    int32_t r=ERR_INTERNAL;
    if (f && !fseek(f,offset,SEEK_SET) &&
        (write ? fwrite(buffer,1,size,f) : fread(buffer,1,size,f))==size) r=0;
    if (f) fclose(f);
    if (!r && write) ++memory_writes;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t memory_get(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,0); }
static ABI int32_t memory_set(int32_t user, void *buffer, uint64_t size, int64_t offset) { (void)user; return memory_io(buffer,size,offset,1); }

static ABI int32_t save_umount_backup(const MountPoint *point) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    pthread_mutex_lock(&lock);
    const int slot=slot_of(point);
    if (slot<0) { pthread_mutex_unlock(&lock); return save_umount(point); }
    runtime_file_unmount(point->data);
    /* Still counted as mounted for writing until the copy is done (the window close waits). */
    /* SOTC_SAVE_BACKUP=0 keeps no backups. */
    const char *env=getenv("SOTC_SAVE_BACKUP");
    const int backed=slots[slot].writable && !(env && env[0]=='0') && !make_backup(slots[slot].host,slots[slot].meta);
    slots[slot].used=0;
    pthread_mutex_unlock(&lock);
    if (getenv("SOTC_SAVE_TRACE"))
        printf("Runtime: sceSaveDataUmountWithBackup(%.16s) -> 0x0%s\n",point->data,backed ? " (backed up)" : "");
    return 0;
}
/* sotc: for the save dialogs (runtime_services.c), which must tell SotC's two YES/NO messages
 * apart: "restore the backup?" comes right after CheckBackupData found one, "the data is
 * corrupted" after it found none or the restore was declined. Restores are counted so that a
 * backup which is broken as well is not restored forever. */
#define RESTORED_MAX 8
static struct { char dir[33]; int found; unsigned seq; } last_check;
static struct { char dir[33]; int count; } restored[RESTORED_MAX];
static unsigned restored_next;
static void note_check(const char *dir, int found) {
    pthread_mutex_lock(&lock);
    snprintf(last_check.dir,sizeof(last_check.dir),"%.32s",dir);
    last_check.found=found; ++last_check.seq;
    pthread_mutex_unlock(&lock);
}
static void note_restore(const char *dir) {
    pthread_mutex_lock(&lock);
    int i=0;
    while (i<RESTORED_MAX && !(restored[i].count && !strncmp(restored[i].dir,dir,32))) ++i;
    if (i==RESTORED_MAX) {
        i=(int)(restored_next++%RESTORED_MAX);
        snprintf(restored[i].dir,sizeof(restored[i].dir),"%.32s",dir);
        restored[i].count=0;
    }
    ++restored[i].count;
    pthread_mutex_unlock(&lock);
}
/* OrbisSaveDataCheckBackupData / RestoreBackupData: user, pad, title, dir, then (check) param, icon. */
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; Param *param; Icon *icon;
                 uint8_t reserved[32]; } CheckBackup;
typedef struct { int32_t user; int32_t pad; const TitleId *title; const DirName *dir; } RestoreBackup;
static int backup_of(int32_t user, const TitleId *title, const DirName *dir, char *host, char *meta, char *backup) {
    if (!dir || !valid_name(dir->data,sizeof(dir->data))) return -1;
    char base[600];
    root(user,title ? title->data : NULL,base,sizeof(base));
    snprintf(host,640,"%s/%s",base,dir->data);
    snprintf(meta,680,"%s/%s.sce_sys",base,dir->data);
    backup_paths(host,backup,720);
    return 0;
}
static ABI int32_t save_check_backup(const CheckBackup *c) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    char host[640], meta[680], backup[720], part[760];
    if (!c || backup_of(c->user,c->title,c->dir,host,meta,backup)) return ERR_PARAMETER;
    struct stat st;
    snprintf(part,sizeof(part),"%s/data",backup);
    const int found=!stat(part,&st) && S_ISDIR(st.st_mode);
    if (found) {
        snprintf(part,sizeof(part),"%s/meta",backup);
        if (c->param) read_param(part,c->param);
        if (c->icon && c->icon->buffer) {
            char icon[800]; snprintf(icon,sizeof(icon),"%s/icon0.png",part);
            FILE *f=fopen(icon,"rb");
            Icon *out=c->icon;
            out->data_size=f ? fread((void *)out->buffer,1,out->buffer_size,f) : 0;
            if (f) fclose(f);
        }
    }
    note_check(c->dir->data,found);
    printf("Runtime: sceSaveDataCheckBackupData('%.32s') -> %s\n",c->dir->data,found ? "found" : "none");
    return found ? 0 : ERR_NOT_FOUND;
}
static ABI int32_t save_restore_backup(const RestoreBackup *r) {
    if (!initialized) return ERR_NOT_INITIALIZED;
    char host[640], meta[680], backup[720], part[760], tmp[700];
    if (!r || backup_of(r->user,r->title,r->dir,host,meta,backup)) return ERR_PARAMETER;
    struct stat st;
    snprintf(part,sizeof(part),"%s/data",backup);
    if (stat(part,&st)) return ERR_NOT_FOUND;
    pthread_mutex_lock(&lock);
    int busy=0;
    for (int i=0;i<SLOTS;++i) if (slots[i].used && !strcmp(slots[i].host,host)) busy=1;
    int32_t result=busy ? ERR_BUSY : 0;
    if (!busy) {
        /* Data first (to a temporary beside it, then swapped in), then the metadata. */
        snprintf(tmp,sizeof(tmp),"%s.restore",host);
        remove_tree(tmp);
        if (copy_tree(part,tmp) || (remove_tree(host),move_dir(tmp,host))) result=ERR_INTERNAL;
        snprintf(part,sizeof(part),"%s/meta",backup);
        snprintf(tmp,sizeof(tmp),"%s.restore",meta);
        remove_tree(tmp);
        if (!result && (copy_tree(part,tmp) || (remove_tree(meta),move_dir(tmp,meta)))) result=ERR_INTERNAL;
        if (result) remove_tree(tmp);
    }
    pthread_mutex_unlock(&lock);
    if (!result) note_restore(r->dir->data);
    printf("Runtime: sceSaveDataRestoreBackupData('%.32s') -> 0x%x\n",r->dir->data,(unsigned)result);
    return result;
}
/* SceSaveDataMountInfo: blocks, freeBlocks (32 KiB each), reserved[32]. */
static ABI int32_t save_mount_info(const MountPoint *mount_point, uint64_t *info) {
    if (!mount_point || !info) return (int32_t)0x809F0001;
    pthread_mutex_lock(&lock);
    const int slot=slot_of(mount_point);
    if (slot<0) { pthread_mutex_unlock(&lock); return ERR_NOT_MOUNTED; }
    space(slots[slot].host,slots[slot].meta,&info[0],&info[1]);
    pthread_mutex_unlock(&lock);
    memset(info+2,0,32);
    if (getenv("SOTC_SAVE_TRACE")) printf("Runtime: sceSaveDataGetMountInfo(%.16s) -> %llu blocks, %llu free\n",mount_point->data,
                                          (unsigned long long)info[0],(unsigned long long)info[1]);
    return 0;
}
/* sotc: for the save/load slot list (runtime_services.c). The param of a save (zeroed when it
 * has none) and a display line: subtitle, detail and date, or the directory name. */
/* sotc: the save's own picture (<dir>.sce_sys/icon0.png, written by sceSaveDataSaveIcon), for the
 * save list; 0 when it exists. */
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) {
    char base[600];
    struct stat st;
    root(user,NULL,base,sizeof(base));
    snprintf(out,size,"%s/%.32s.sce_sys/icon0.png",base,dir);
    return stat(out,&st) ? -1 : 0;
}
int runtime_savedata_param(int32_t user, const char *dir, void *param) {
    char base[600], meta[700];
    root(user,NULL,base,sizeof(base));
    snprintf(meta,sizeof(meta),"%s/%.32s.sce_sys",base,dir);
    return read_param(meta,(Param *)param);
}
void runtime_savedata_describe(int32_t user, const char *dir, char *out, size_t size, int64_t *mtime) {
    Param p;
    const int ok=!runtime_savedata_param(user,dir,&p);
    char when[32]="";
    if (ok && p.mtime) {
        time_t t=(time_t)p.mtime; struct tm tm;
#ifdef _WIN32
        localtime_s(&tm,&t);
#else
        localtime_r(&t,&tm);
#endif
        strftime(when,sizeof(when),"%d/%m/%Y %H:%M",&tm);
    }
    if (mtime) *mtime=ok ? p.mtime : 0;
    if (ok && (p.subtitle[0] || p.detail[0]))
        snprintf(out,size,"%.127s\n%.200s    %s",p.subtitle,p.detail,when);
    else
        snprintf(out,size,"%.32s    %s",dir,when);
}
/* sotc: deletes one save (the slot list's Delete). 0 when it existed. */
int runtime_savedata_remove(int32_t user, const char *dir) {
    char name[33]; snprintf(name,sizeof(name),"%.32s",dir);
    if (!valid_name(name,sizeof(name))) return -1;
    char base[600], host[700], meta[700];
    root(user,NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,name);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,name);
    struct stat st;
    if (stat(host,&st)) return -1;
    remove_tree(host);
    remove_tree(meta);
    char backup[720]; backup_paths(host,backup,sizeof(backup)); remove_tree(backup);
    printf("Runtime: save data '%s' deleted from the slot list\n",name);
    return 0;
}
/* sotc: the save dialogs. 1/0: the last CheckBackupData was for `dir` and found / did not find
 * a backup (*seq numbers the checks); -1: it was for another save, or there was none. */
int runtime_savedata_last_check(const char *dir, unsigned *seq) {
    pthread_mutex_lock(&lock);
    const int r=last_check.seq && !strncmp(last_check.dir,dir,32) ? last_check.found : -1;
    if (seq) *seq=last_check.seq;
    pthread_mutex_unlock(&lock);
    return r;
}
/* Backups of `dir` restored in this session. */
int runtime_savedata_restored(const char *dir) {
    int n=0;
    pthread_mutex_lock(&lock);
    for (int i=0;i<RESTORED_MAX;++i) if (restored[i].count && !strncmp(restored[i].dir,dir,32)) n=restored[i].count;
    pthread_mutex_unlock(&lock);
    return n;
}
/* Copies a save the game calls corrupted (data, metadata and backup) to
 * <title>.quarantine/<dir>-<date> beside the title's saves, where DirNameSearch never looks.
 * 0 when the data was copied; `out` names the copy. */
int runtime_savedata_quarantine(int32_t user, const char *dir, char *out, size_t size) {
    char name[33]; snprintf(name,sizeof(name),"%.32s",dir);
    if (!valid_name(name,sizeof(name))) return -1;
    char base[600], host[700], meta[700], backup[720], dest[760], part[800], when[32];
    root(user,NULL,base,sizeof(base));
    snprintf(host,sizeof(host),"%s/%s",base,name);
    snprintf(meta,sizeof(meta),"%s/%s.sce_sys",base,name);
    backup_paths(host,backup,sizeof(backup));
    struct stat st;
    if (stat(host,&st)) return -1;
    time_t now=time(NULL); struct tm tm;
#ifdef _WIN32
    localtime_s(&tm,&now);
#else
    localtime_r(&now,&tm);
#endif
    strftime(when,sizeof(when),"%Y%m%d-%H%M%S",&tm);
    snprintf(dest,sizeof(dest),"%s.quarantine/%s-%s",base,name,when);
    snprintf(part,sizeof(part),"%s/data",dest);
    const int r=copy_tree(host,part);
    snprintf(part,sizeof(part),"%s/meta",dest);
    if (!stat(meta,&st) && copy_tree(meta,part)) fprintf(stderr,"Runtime: could not copy %s\n",meta);
    snprintf(part,sizeof(part),"%s/backup",dest);
    if (!stat(backup,&st) && copy_tree(backup,part)) fprintf(stderr,"Runtime: could not copy %s\n",backup);
    if (out && size) snprintf(out,size,"%s",dest);
    return r;
}
static const RuntimeExport exports[]={
    {"sceSaveDataInitialize",save_initialize}, {"sceSaveDataInitialize2",save_initialize},
    {"sceSaveDataInitialize3",save_initialize}, {"sceSaveDataTerminate",save_terminate},
    {"sceSaveDataMount",save_mount}, {"sceSaveDataMount2",save_mount2}, {"sceSaveDataUmount",save_umount},
    {"sceSaveDataUmountWithBackup",save_umount_backup}, {"sceSaveDataCheckBackupData",save_check_backup},
    {"sceSaveDataRestoreBackupData",save_restore_backup}, {"sceSaveDataGetMountInfo",save_mount_info},
    {"sceSaveDataSetParam",save_set_param}, {"sceSaveDataSaveIcon",save_icon}, {"sceSaveDataDelete",save_delete},
    {"sceSaveDataDirNameSearch",save_search},
    {"sceSaveDataSetupSaveDataMemory",memory_setup}, {"sceSaveDataGetSaveDataMemory",memory_get},
    {"sceSaveDataSetSaveDataMemory",memory_set},
};
uintptr_t runtime_savedata_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
/* sotc: number of save directories mounted for writing right now (the window close waits for 0). */
int runtime_savedata_mounted(void) {
    int n=0;
    pthread_mutex_lock(&lock);
    for (int i=0;i<SLOTS;++i) n+=slots[i].used && slots[i].writable;
    pthread_mutex_unlock(&lock);
    return n;
}
void runtime_savedata_report(void) { printf("Runtime: save data mounts=%zu, memory writes=%zu\n",mounts_done,memory_writes); }

