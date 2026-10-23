#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#include "ui_settings.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void ui_settings_defaults(ui_settings_t *s) {
    memset(s, 0, sizeof(*s));
    s->side = 1; s->height_permille = 550; s->opacity = 60;
    s->default_profile = -1; s->companion_port = 45850;
}

int ui_settings_valid(const ui_settings_t *s) {
    unsigned char addr[16];
    if (!s || s->side < 0 || s->side > 1 || s->height_permille < 0 ||
        s->height_permille > 1000 || s->opacity < 30 || s->opacity > 85 ||
        s->edge_handle < 0 || s->edge_handle > 1 || s->default_profile < -1 ||
        s->default_profile > 2 || s->companion_enabled < 0 || s->companion_enabled > 1 ||
        s->companion_port < 1 || s->companion_port > 65535 ||
        !memchr(s->companion_host, 0, sizeof(s->companion_host)) ||
        !memchr(s->companion_token, 0, sizeof(s->companion_token))) return 0;
    if (s->companion_host[0] &&
        inet_pton(AF_INET, s->companion_host, addr) != 1 &&
        inet_pton(AF_INET6, s->companion_host, addr) != 1) return 0;
    size_t key_length = strlen(s->companion_token);
    if (s->companion_enabled && (!s->companion_host[0] || !key_length)) return 0;
    if (key_length && key_length != 64) return 0;
    for (const unsigned char *p = (const unsigned char *)s->companion_token; *p; ++p)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return 0;
    return 1;
}

static int integer(const char *p, int *out) {
    char *end; errno = 0;
    if (!*p || (*p != '-' && (*p < '0' || *p > '9'))) return -1;
    long n = strtol(p, &end, 10);
    if (errno || *end || n < -1 || n > 65535) return -1;
    *out = (int)n; return 0;
}

static int path_for(const char *dir, const char *name, char *out, size_t cap) {
    if (!dir || !*dir) return -1;
    int n = snprintf(out, cap, "%s/%s", dir, name);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int ui_settings_load(const char *directory, ui_settings_t *out) {
    ui_settings_t candidate;
    char path[1200], line[384]; unsigned seen = 0; int version = 0;
    ui_settings_defaults(out); ui_settings_defaults(&candidate);
    if (path_for(directory, "ui-settings.conf", path, sizeof(path))) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 4096 ||
        st.st_uid != geteuid() || (st.st_mode & 077)) { close(fd); return -1; }
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    int rc = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'), *eq = strchr(line, '=');
        if (!nl || !eq || eq > nl) { rc = -1; break; }
        *nl = 0; *eq++ = 0;
        int bit = -1, *dst = NULL;
        if (!strcmp(line, "version")) { bit=0; dst=&version; }
        else if (!strcmp(line,"side")) { bit=1; dst=&candidate.side; }
        else if (!strcmp(line,"height")) { bit=2; dst=&candidate.height_permille; }
        else if (!strcmp(line,"opacity")) { bit=3; dst=&candidate.opacity; }
        else if (!strcmp(line,"edge")) { bit=4; dst=&candidate.edge_handle; }
        else if (!strcmp(line,"default_profile")) { bit=5; dst=&candidate.default_profile; }
        else if (!strcmp(line,"companion_enabled")) { bit=6; dst=&candidate.companion_enabled; }
        else if (!strcmp(line,"companion_port")) { bit=7; dst=&candidate.companion_port; }
        else if (!strcmp(line,"companion_host")) {
            bit=8; if (strlen(eq)>=sizeof(candidate.companion_host)) { rc=-1;break; }
            strcpy(candidate.companion_host,eq);
        } else if (!strcmp(line,"companion_token")) {
            bit=9; if (strlen(eq)>=sizeof(candidate.companion_token)) { rc=-1;break; }
            strcpy(candidate.companion_token,eq);
        }
        if (bit<0 || (seen & (1u<<bit)) || (dst && integer(eq,dst))) { rc=-1;break; }
        seen |= 1u<<bit;
    }
    if (ferror(f) || version != 1 || seen != 1023 || !ui_settings_valid(&candidate)) rc=-1;
    fclose(f);
    if (!rc) *out=candidate;
    memset(&candidate,0,sizeof(candidate));
    return rc;
}

int ui_settings_save(const char *directory, const ui_settings_t *s) {
    char path[1200], temp[1200]; struct stat st;
    if (!ui_settings_valid(s) || path_for(directory,"ui-settings.conf",path,sizeof(path)) ||
        path_for(directory,".ui-settings-XXXXXX",temp,sizeof(temp))) return -1;
    if (mkdir(directory,0700) && errno != EEXIST) return -1;
    if (lstat(directory,&st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() ||
        chmod(directory,0700)) return -1;
    int fd=mkstemp(temp);
    if(fd<0) return -1;
    if(fchmod(fd,0600)) {close(fd);unlink(temp);return -1;}
    FILE *f=fdopen(fd,"w");
    if(!f) {close(fd);unlink(temp);return -1;}
    /* Authentication stays in the same private 0600 transaction as the
     * endpoint. It never goes to Documents, reference files, or logs. */
    int rc=fprintf(f,"version=1\nside=%d\nheight=%d\nopacity=%d\nedge=%d\ndefault_profile=%d\n"
        "companion_enabled=%d\ncompanion_port=%d\ncompanion_host=%s\ncompanion_token=%s\n",
        s->side,s->height_permille,s->opacity,s->edge_handle,s->default_profile,
        s->companion_enabled,s->companion_port,s->companion_host,s->companion_token)<0 ? -1:0;
    if(fflush(f) || fsync(fd)) rc=-1;
    if(fclose(f)) rc=-1;
    if(!rc && rename(temp,path)) rc=-1;
    if(rc) unlink(temp);
    return rc;
}
