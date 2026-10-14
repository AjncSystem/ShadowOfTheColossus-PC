/* Offline AppContent provider for the configured base-game profile.
 * No package/DLC mounting, downloads, entitlement or license emulation. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#endif
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static void scan_addons(void);
static unsigned addon_count;
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    scan_addons();
    printf("Runtime: AppContent initialized; offline base-game profile, %u add-on(s) installed\n",addon_count);
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
/* sotc: installed add-on content (DLC) from SOTC_DLC_DIR, as shadPS4 does: the folder (or its
 * <TITLE_ID> subfolder) holds one folder per add-on with sce_sys/param.sfo, CATEGORY "ac..." and a
 * CONTENT_ID "UPXXXX-CUSAXXXXX_XX-<entitlement label>". The game only lists them (entitlements);
 * nothing is mounted. */
#define MAX_ADDONS 64
typedef struct { char label[17]; char padding[3]; uint32_t status; } AddonInfo; /* 24 bytes */
static AddonInfo addons[MAX_ADDONS];
static unsigned addons_scanned;
/* The string value of key in a param.sfo image, or NULL. */
static const char *sfo_string(const unsigned char *data, size_t size, const char *key) {
    if (size<20 || memcmp(data,"\0PSF",4)) return NULL;
    uint32_t keys, values, count;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&count,data+16,4);
    for (uint32_t i=0; i<count && 20+i*16+16<=size; ++i) {
        const unsigned char *e=data+20+i*16;
        uint16_t key_off, fmt; uint32_t len, off;
        memcpy(&key_off,e,2); memcpy(&fmt,e+2,2); memcpy(&len,e+4,4); memcpy(&off,e+12,4);
        if (keys+key_off>=size || values+off+len>size || fmt==0x404) continue;
        if (!strncmp((const char *)data+keys+key_off,key,size-keys-key_off) && len) {
            const char *value=(const char *)data+values+off;
            return memchr(value,0,len) ? value : NULL;
        }
    }
    return NULL;
}
/* True when the entitlement of content ID is listed already (a package and its installed folder). */
static int addon_known(const char *content) {
    for (unsigned i=0;i<addon_count;++i) if (!strncmp(addons[i].label,content+20,16)) return 1;
    return 0;
}
static void scan_addons(void) {
    if (addons_scanned) return;
    addons_scanned=1;
#ifndef _WIN32
    /* Same rules as the Windows scan below: <root>/CUSA08809 or <root>, one folder per add-on
     * (sce_sys/param.sfo) or an add-on .pkg. */
    const char *root=getenv("SOTC_DLC_DIR");
    if (!root || !*root) return;
    static char base[1024], path[1400];
    struct stat st;
    snprintf(base,sizeof base,"%s/CUSA08809",root);
    if (stat(base,&st) || !S_ISDIR(st.st_mode)) snprintf(base,sizeof base,"%s",root);
    DIR *dir=opendir(base);
    if (!dir) { printf("Runtime: DLC folder %s not found\n",base); return; }
    struct dirent *entry;
    while ((entry=readdir(dir))) {
        if (entry->d_name[0]=='.') continue;
        snprintf(path,sizeof path,"%s/%s",base,entry->d_name);
        if (stat(path,&st)) continue;
        const size_t name_len=strlen(entry->d_name);
        if (S_ISREG(st.st_mode) && name_len>4 && !strcasecmp(entry->d_name+name_len-4,".pkg")) {
            FILE *pkg=fopen(path,"rb");
            static unsigned char header[0x80];
            const size_t got=pkg ? fread(header,1,sizeof header,pkg) : 0;
            if (pkg) fclose(pkg);
            static char content[37];
            memcpy(content,header+0x40,36); content[36]=0;
            const uint32_t type=(uint32_t)header[0x74]<<24|header[0x75]<<16|header[0x76]<<8|header[0x77];
            if (got<sizeof header || memcmp(header,"\x7f" "CNT",4) || type!=0x1b ||
                !strstr(content,"CUSA08809") || strlen(content)<=20 || addon_count==MAX_ADDONS ||
                addon_known(content)) continue;
            AddonInfo *info=&addons[addon_count++];
            memset(info,0,sizeof *info);
            snprintf(info->label,sizeof info->label,"%s",content+20);
            info->status=4;
            printf("Runtime: DLC %s: entitlement %s\n",entry->d_name,info->label);
            continue;
        }
        if (!S_ISDIR(st.st_mode)) continue;
        snprintf(path,sizeof path,"%s/%s/sce_sys/param.sfo",base,entry->d_name);
        FILE *f=fopen(path,"rb");
        if (!f) continue;
        static unsigned char data[65536]; /* not on the guest thread's small stack */
        const size_t size=fread(data,1,sizeof data,f);
        fclose(f);
        const char *category=sfo_string(data,size,"CATEGORY");
        const char *content=sfo_string(data,size,"CONTENT_ID");
        if (!category || strncmp(category,"ac",2) || !content || strlen(content)<=20) {
            printf("Runtime: DLC %s skipped (not add-on content)\n",entry->d_name);
            continue;
        }
        if (addon_count==MAX_ADDONS) break;
        if (addon_known(content)) continue;
        AddonInfo *info=&addons[addon_count++];
        memset(info,0,sizeof *info);
        snprintf(info->label,sizeof info->label,"%s",content+20);
        info->status=4; /* installed */
        printf("Runtime: DLC %s: entitlement %s\n",entry->d_name,info->label);
    }
    closedir(dir);
#else
    const char *root=getenv("SOTC_DLC_DIR");
    if (!root || !*root) return;
    static char base[1024], pattern[1100];
    snprintf(base,sizeof base,"%s\\CUSA08809",root);
    if (GetFileAttributesA(base)==INVALID_FILE_ATTRIBUTES) snprintf(base,sizeof base,"%s",root);
    snprintf(pattern,sizeof pattern,"%s\\*",base);
    static WIN32_FIND_DATAA entry;
    HANDLE find=FindFirstFileA(pattern,&entry);
    if (find==INVALID_HANDLE_VALUE) { printf("Runtime: DLC folder %s not found\n",base); return; }
    do {
        if (entry.cFileName[0]=='.') continue;
        static char path[1400];
        /* An add-on package not installed: its header says it is additional content (type
         * 0x1b) and carries the content ID. Entitlement-only DLC needs nothing else. */
        const size_t name_len=strlen(entry.cFileName);
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name_len>4 &&
            !_stricmp(entry.cFileName+name_len-4,".pkg")) {
            snprintf(path,sizeof path,"%s\\%s",base,entry.cFileName);
            FILE *pkg=fopen(path,"rb");
            static unsigned char header[0x80];
            const size_t got=pkg ? fread(header,1,sizeof header,pkg) : 0;
            if (pkg) fclose(pkg);
            static char content[37];
            memcpy(content,header+0x40,36); content[36]=0;
            const uint32_t type=(uint32_t)header[0x74]<<24|header[0x75]<<16|header[0x76]<<8|header[0x77];
            if (got<sizeof header || memcmp(header,"\x7f" "CNT",4) || type!=0x1b ||
                !strstr(content,"CUSA08809") || strlen(content)<=20 || addon_count==MAX_ADDONS ||
                addon_known(content)) continue;
            AddonInfo *info=&addons[addon_count++];
            memset(info,0,sizeof *info);
            snprintf(info->label,sizeof info->label,"%s",content+20);
            info->status=4;
            printf("Runtime: DLC %s: entitlement %s\n",entry.cFileName,info->label);
            continue;
        }
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        snprintf(path,sizeof path,"%s\\%s\\sce_sys\\param.sfo",base,entry.cFileName);
        FILE *f=fopen(path,"rb");
        if (!f) continue;
        static unsigned char data[65536]; /* not on the guest thread's small stack */
        const size_t size=fread(data,1,sizeof data,f);
        fclose(f);
        const char *category=sfo_string(data,size,"CATEGORY");
        const char *content=sfo_string(data,size,"CONTENT_ID");
        if (!category || strncmp(category,"ac",2) || !content || strlen(content)<=20) {
            printf("Runtime: DLC %s skipped (not add-on content)\n",entry.cFileName);
            continue;
        }
        if (addon_count==MAX_ADDONS) break;
        if (addon_known(content)) continue;
        AddonInfo *info=&addons[addon_count++];
        memset(info,0,sizeof *info);
        snprintf(info->label,sizeof info->label,"%s",content+20);
        info->status=4; /* installed */
        printf("Runtime: DLC %s: entitlement %s\n",entry.cFileName,info->label);
    } while (FindNextFileA(find,&entry));
    FindClose(find);
#endif
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    scan_addons();
    if (!capacity || !list) { *hits=addon_count; ++lists; return 0; }
    const unsigned n=addon_count<capacity ? addon_count : capacity;
    /* Never touch list slots past the add-ons. */
    if (n) memcpy(list,addons,n*sizeof(AddonInfo));
    if (hits) *hits=n;
    ++lists; return 0;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk")) return (uintptr_t)addon_list;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u\n",refs,initialized,queries,lists);
}
