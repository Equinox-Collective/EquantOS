// userspace/epacmg.c - EquantOS Package Manager (OPM) Client
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/syscall.h>
#include <stdint.h>
#include <stdbool.h>

#include <bearssl.h>

#define SYS_EQUANT_DNS 401

#define DEFAULT_TRANS_HOST "raw.githubusercontent.com"
#define DEFAULT_MANIFEST_PATH "/ewasion137/epacmg-trans/main/packages.txt"

#define DB_DIR "/var/lib/epacmg"
#define DB_INSTALLED "/var/lib/epacmg/installed.db"
#define DB_CACHE "/var/lib/epacmg/packages.db"

// Direct synchronous logger (bypasses stdio buffering, visible before any crash).
// Step tracing only with EPACMG_DEBUG=1; errors are always shown.
static void dbg(const char *msg) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("EPACMG_DEBUG") != NULL;
    if (enabled || strncmp(msg, "[ERROR]", 7) == 0) write(1, msg, strlen(msg));
}

// Download progress on one terminal line, about every 256 KB
static void progress(size_t received, bool done) {
    static size_t shown = 0;
    if (!done && received - shown < 262144 && received >= shown) return;
    shown = received;
    printf("\r  %zu KB received%s", received / 1024, done ? "\n" : "");
    fflush(stdout);
    if (done) shown = 0;
}

static int resolve_dns(const char *host, uint32_t *ip) {
    return syscall(SYS_EQUANT_DNS, host, ip);
}

// ============================================================================
// Built-in POSIX TAR Extractor (Extracts .epkg directly to /)
// ============================================================================

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed)) tar_header_t;

static unsigned int parse_octal(const char *str, int len) {
    unsigned int val = 0;
    while (len > 0 && (*str == ' ' || *str == '0')) { str++; len--; }
    while (len > 0 && *str >= '0' && *str <= '7') {
        val = (val << 3) | (*str++ - '0');
        len--;
    }
    return val;
}

static void create_parent_dirs(const char *path) {
    char temp[256];
    strncpy(temp, path, sizeof(temp) - 1);
    char *p = temp;
    if (*p == '/') p++;
    while ((p = strchr(p, '/')) != NULL) {
        *p = '\0';
        mkdir(temp, 0755);
        *p = '/';
        p++;
    }
}

static int extract_tar(const uint8_t *tar_data, size_t total_size) {
    size_t offset = 0;
    int files_extracted = 0;

    while (offset + 512 <= total_size) {
        const tar_header_t *hdr = (const tar_header_t *)(tar_data + offset);
        if (hdr->name[0] == '\0') break;

        unsigned int file_size = parse_octal(hdr->size, sizeof(hdr->size));
        offset += 512;

        char name[256];
        if (hdr->prefix[0] && memcmp(hdr->magic, "ustar", 5) == 0) {
            snprintf(name, sizeof(name), "%.155s/%.100s", hdr->prefix, hdr->name);
        } else {
            snprintf(name, sizeof(name), "%.100s", hdr->name);
        }
        const char *rel = name;
        while (rel[0] == '.' && rel[1] == '/') rel += 2;

        char dest_path[256];
        if (rel[0] == '/') {
            snprintf(dest_path, sizeof(dest_path), "%s", rel);
        } else {
            snprintf(dest_path, sizeof(dest_path), "/%s", rel);
        }
        if (offset + file_size > total_size) break; // truncated archive

        if (hdr->typeflag == '5') {
            mkdir(dest_path, 0755);
        } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            create_parent_dirs(dest_path);
            printf("  -> Extracting: %s (%u bytes)\n", dest_path, file_size);
            int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
            if (fd >= 0) {
                size_t done = 0;
                while (done < file_size) {
                    ssize_t w = write(fd, tar_data + offset + done, file_size - done);
                    if (w <= 0) break;
                    done += (size_t)w;
                }
                close(fd);
                chmod(dest_path, 0755);
                files_extracted++;
            }
        }
        offset += (file_size + 511) & ~511;
    }
    return files_extracted;
}

// ============================================================================
// BearSSL No-Anchor Insecure Adapter
// ============================================================================

typedef struct {
    const br_x509_class *vtable;
    const br_x509_class **inner;
} x509_noanchor_context;

static void xwc_start_chain(const br_x509_class **ctx, const char *server_name) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->start_chain(xwc->inner, server_name);
}

static void xwc_start_cert(const br_x509_class **ctx, uint32_t length) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->start_cert(xwc->inner, length);
}

static void xwc_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->append(xwc->inner, buf, len);
}

static void xwc_end_cert(const br_x509_class **ctx) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->end_cert(xwc->inner);
}

static unsigned xwc_end_chain(const br_x509_class **ctx) {
    x509_noanchor_context *xwc = (x509_noanchor_context *)ctx;
    (*xwc->inner)->end_chain(xwc->inner);
    return BR_ERR_OK; // Unconditionally trust server certificate
}

static const br_x509_pkey *xwc_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    const x509_noanchor_context *xwc = (const x509_noanchor_context *)ctx;
    return (*xwc->inner)->get_pkey(xwc->inner, usages);
}

static const br_x509_class x509_noanchor_vtable = {
    sizeof(x509_noanchor_context),
    xwc_start_chain,
    xwc_start_cert,
    xwc_append,
    xwc_end_cert,
    xwc_end_chain,
    xwc_get_pkey
};

// ============================================================================
// BearSSL HTTPS / HTTP Engine with Full Debug Tracing
// ============================================================================

static int sock_read_cb(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    char b[64];
    snprintf(b, sizeof(b), "  [CB:READ] requesting up to %zu bytes from fd %d...\n", len, fd);
    dbg(b);

    ssize_t r = read(fd, buf, len);
    snprintf(b, sizeof(b), "  [CB:READ] returned %zd bytes\n", r);
    dbg(b);

    if (r <= 0) return -1;
    return (int)r;
}

static int sock_write_cb(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    char b[64];
    snprintf(b, sizeof(b), "  [CB:WRITE] writing %zu bytes to fd %d...\n", len, fd);
    dbg(b);

    ssize_t w = write(fd, buf, len);
    snprintf(b, sizeof(b), "  [CB:WRITE] returned %zd bytes\n", w);
    dbg(b);

    if (w <= 0) return -1;
    return (int)w;
}

static br_ssl_client_context sc;
static br_x509_minimal_context xc;
static x509_noanchor_context xwc;
static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];

static uint8_t *http_fetch(const char *host, int port, const char *path, bool use_ssl, size_t *out_size) {
    uint32_t ip = 0;
    dbg("[STEP 1] Resolving host via DNS...\n");
    if (resolve_dns(host, &ip) != 0 || ip == 0) {
        dbg("[ERROR] DNS resolution failed\n");
        return NULL;
    }

    dbg("[STEP 2] Creating TCP socket...\n");
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        dbg("[ERROR] socket() failed\n");
        return NULL;
    }

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(ip);

    dbg("[STEP 3] Connecting to remote server...\n");
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        dbg("[ERROR] connect() failed\n");
        close(fd);
        return NULL;
    }
    dbg("[STEP 4] TCP Connected successfully!\n");

    char req[512];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: epacmg/1.0 (EquantOS)\r\n"
             "Connection: close\r\n\r\n",
             path, host);

    size_t buf_cap = 1024 * 1024;
    uint8_t *buf = (uint8_t *)malloc(buf_cap);
    size_t received = 0;

    if (!use_ssl) {
        dbg("[STEP 5] Plain HTTP path selected.\n");
        write(fd, req, strlen(req));
        ssize_t r;
        while ((r = read(fd, buf + received, buf_cap - received - 1)) > 0) {
            received += r;
            progress(received, false);
            if (received + 4096 >= buf_cap) {
                buf_cap *= 2;
                buf = (uint8_t *)realloc(buf, buf_cap);
            }
        }
        progress(received, true);
    } else {
        dbg("[STEP 5] Setting up BearSSL static structures...\n");

        dbg("[STEP 6] Calling br_ssl_client_init_full()...\n");
        br_ssl_client_init_full(&sc, &xc, NULL, 0);
        dbg("[STEP 6] br_ssl_client_init_full() done.\n");

        dbg("[STEP 7] Attaching X509 No-Anchor engine & buffer...\n");
        xwc.vtable = &x509_noanchor_vtable;
        xwc.inner = &xc.vtable;
        br_ssl_engine_set_x509(&sc.eng, &xwc.vtable);
        br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof(iobuf), 1);

        dbg("[STEP 8] Injecting CPU entropy...\n");
        uint8_t entropy[32];
        uint32_t tsc_lo, tsc_hi;
        __asm__ volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
        uint64_t tsc = ((uint64_t)tsc_hi << 32) | tsc_lo;
        for (int i = 0; i < 32; i++) {
            tsc = tsc * 6364136223846793005ULL + 1442695040888963407ULL;
            entropy[i] = (uint8_t)(tsc >> (i % 8 * 8));
        }
        br_ssl_engine_inject_entropy(&sc.eng, entropy, sizeof(entropy));

        dbg("[STEP 9] Calling br_ssl_client_reset()...\n");
        int reset_res = br_ssl_client_reset(&sc, host, 0);
        char r_msg[64];
        snprintf(r_msg, sizeof(r_msg), "[STEP 9] Reset returned: %d (last_err=%d)\n",
                 reset_res, br_ssl_engine_last_error(&sc.eng));
        dbg(r_msg);

        if (reset_res == 0) {
            dbg("[ERROR] br_ssl_client_reset failed!\n");
            close(fd);
            free(buf);
            return NULL;
        }

        dbg("[STEP 10] Initializing br_sslio bridge...\n");
        br_sslio_context ioc;
        br_sslio_init(&ioc, &sc.eng, sock_read_cb, &fd, sock_write_cb, &fd);

        dbg("[STEP 11] Sending HTTPS GET Request through br_sslio_write_all()...\n");
        br_sslio_write_all(&ioc, req, strlen(req));
        dbg("[STEP 11] br_sslio_write_all() completed!\n");

        dbg("[STEP 12] Flushing TLS pipeline with br_sslio_flush()...\n");
        br_sslio_flush(&ioc);
        dbg("[STEP 12] Flush completed!\n");

        dbg("[STEP 13] Reading HTTPS Response...\n");
        for (;;) {
            int r = br_sslio_read(&ioc, buf + received, buf_cap - received - 1);
            if (r < 0) break;
            received += r;
            progress(received, false);
            if (received + 4096 >= buf_cap) {
                buf_cap *= 2;
                buf = (uint8_t *)realloc(buf, buf_cap);
            }
        }
        progress(received, true);
        dbg("[STEP 14] Read loop terminated.\n");
    }

    close(fd);

    if (received == 0) {
        char err_msg[64];
        snprintf(err_msg, sizeof(err_msg), "[ERROR] Received 0 bytes (SSL engine error: %d)\n",
                 br_ssl_engine_last_error(&sc.eng));
        dbg(err_msg);
        free(buf);
        return NULL;
    }

    buf[received] = '\0';

    char *body = strstr((char *)buf, "\r\n\r\n");
    if (!body) {
        dbg("[ERROR] No HTTP header/body delimiter found.\n");
        free(buf);
        return NULL;
    }
    body += 4;

    int status = 0;
    if (sscanf((char *)buf, "HTTP/%*d.%*d %d", &status) != 1 || status != 200) {
        char msg[96];
        snprintf(msg, sizeof(msg), "[ERROR] Server answered HTTP %d for %s\n", status, path);
        dbg(msg);
        free(buf);
        return NULL;
    }

    size_t body_len = received - (size_t)(body - (char *)buf);
    uint8_t *res = (uint8_t *)malloc(body_len);
    memcpy(res, body, body_len);
    free(buf);

    *out_size = body_len;
    return res;
}

// ============================================================================
// CLI Commands & Actions
// ============================================================================

static void parse_url(const char *url, char *host, int *port, char *path, bool *is_ssl) {
    *is_ssl = false;
    *port = 80;

    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) {
        *is_ssl = true;
        *port = 443;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    const char *slash = strchr(p, '/');
    if (slash) {
        size_t hlen = slash - p;
        strncpy(host, p, hlen);
        host[hlen] = '\0';
        strcpy(path, slash);
    } else {
        strcpy(host, p);
        strcpy(path, "/");
    }
}

static int cmd_sync(void) {
    mkdir("/var", 0755);
    mkdir("/var/lib", 0755);
    mkdir(DB_DIR, 0755);

    printf("\033[36m:: Synchronizing package databases...\033[0m\n");

    size_t size = 0;
    uint8_t *data = http_fetch(DEFAULT_TRANS_HOST, 443, DEFAULT_MANIFEST_PATH, true, &size);
    if (!data) {
        printf("\033[31mError: Failed to fetch repository manifest from %s\033[0m\n", DEFAULT_TRANS_HOST);
        return 1;
    }

    int fd = open(DB_CACHE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("\033[31mError: Failed to create database file %s (errno=%d)\033[0m\n", DB_CACHE, fd);
        free(data);
        return 1;
    }
    write(fd, data, size);
    close(fd);
    free(data);

    printf("\033[32m:: Synchronization complete. Manifest updated (%zu bytes).\033[0m\n", size);
    return 0;
}

static int cmd_install(const char *pkg_name) {
    FILE *f = fopen(DB_CACHE, "r");
    if (!f) {
        printf("Database not found. Running sync first...\n");
        if (cmd_sync() != 0) return 1;
        f = fopen(DB_CACHE, "r");
        if (!f) return 1;
    }

    char line[512];
    bool found = false;
    char name[64], ver[32], size_str[32], url[256], desc[128];

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || strlen(line) < 5) continue;
        char *p = line;
        char *tok = strtok(p, "|\n"); if (!tok) continue; strncpy(name, tok, sizeof(name)-1);
        tok = strtok(NULL, "|\n"); if (!tok) continue; strncpy(ver, tok, sizeof(ver)-1);
        tok = strtok(NULL, "|\n"); if (!tok) continue; strncpy(size_str, tok, sizeof(size_str)-1);
        tok = strtok(NULL, "|\n"); if (!tok) continue; strncpy(url, tok, sizeof(url)-1);
        tok = strtok(NULL, "|\n"); if (!tok) desc[0] = '\0'; else strncpy(desc, tok, sizeof(desc)-1);

        if (strcmp(name, pkg_name) == 0) {
            found = true;
            break;
        }
    }
    fclose(f);

    if (!found) {
        printf("\033[31mError: target not found: %s\033[0m\n", pkg_name);
        return 1;
    }

    printf("\033[36m:: Resolving dependencies...\033[0m\n");
    printf("\033[32mPackage (%s) %s [%s bytes] - %s\033[0m\n", name, ver, size_str, desc);
    printf(":: Proceed with installation? [Y/n] y\n");

    char host[128], path[256];
    int port;
    bool is_ssl;
    parse_url(url, host, &port, path, &is_ssl);

    size_t epkg_size = 0;
    uint8_t *epkg_data = http_fetch(host, port, path, is_ssl, &epkg_size);
    if (!epkg_data) {
        printf("\033[31mError: failed to download package archive from %s\033[0m\n", url);
        return 1;
    }

    printf(":: Extracting %s...\n", name);
    int extracted = extract_tar(epkg_data, epkg_size);
    free(epkg_data);

    if (extracted > 0) {
        FILE *inst = fopen(DB_INSTALLED, "a");
        if (inst) {
            fprintf(inst, "%s|%s\n", name, ver);
            fclose(inst);
        }
        printf("\033[32m(1/1) Successfully installed %s (%s)!\033[0m\n", name, ver);
    } else {
        printf("\033[31mError: archive extracted 0 files (invalid .epkg format)\033[0m\n");
        return 1;
    }

    return 0;
}

// Install a package archive that is already on disk (like pacman -U)
static int cmd_install_file(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("\033[31mError: cannot open %s\033[0m\n", path);
        return 1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        printf("\033[31mError: cannot stat %s\033[0m\n", path);
        close(fd);
        return 1;
    }

    size_t size = (size_t)st.st_size;
    uint8_t *data = (uint8_t *)malloc(size);
    if (!data) {
        printf("\033[31mError: out of memory reading %s\033[0m\n", path);
        close(fd);
        return 1;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, data + got, size - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);

    printf(":: Extracting %s...\n", path);
    int extracted = extract_tar(data, got);
    free(data);
    if (extracted <= 0) {
        printf("\033[31mError: archive extracted 0 files (invalid .epkg format)\033[0m\n");
        return 1;
    }

    mkdir("/var", 0755);
    mkdir("/var/lib", 0755);
    mkdir(DB_DIR, 0755);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char name[64];
    snprintf(name, sizeof(name), "%s", base);
    char *dot = strstr(name, ".epkg");
    if (dot) *dot = '\0';
    FILE *inst = fopen(DB_INSTALLED, "a");
    if (inst) {
        fprintf(inst, "%s|local\n", name);
        fclose(inst);
    }
    printf("\033[32m(1/1) Installed %s (%d files)\033[0m\n", name, extracted);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("EquantOS Package Manager (epacmg) v1.0\n");
        printf("Usage: epacmg <operation> [...]\n\n");
        printf("Operations:\n");
        printf("  sync, -Sy          Update package repository database\n");
        printf("  ins,  -S <pkg>     Install target package from repository\n");
        printf("  -U <file.epkg>     Install a package archive from disk\n");
        printf("  list, -Q           List available packages\n");
        return 0;
    }

    if (strcmp(argv[1], "sync") == 0 || strcmp(argv[1], "-Sy") == 0) {
        return cmd_sync();
    } else if ((strcmp(argv[1], "ins") == 0 || strcmp(argv[1], "-S") == 0) && argc >= 3) {
        return cmd_install(argv[2]);
    } else if (strcmp(argv[1], "-U") == 0 && argc >= 3) {
        return cmd_install_file(argv[2]);
    } else if (strcmp(argv[1], "list") == 0 || strcmp(argv[1], "-Q") == 0) {
        FILE *f = fopen(DB_CACHE, "r");
        if (!f) { printf("No cache. Run 'epacmg sync' first.\n"); return 1; }
        char line[256];
        printf("=== Available Packages ===\n");
        while (fgets(line, sizeof(line), f)) printf("  %s", line);
        fclose(f);
        return 0;
    }

    printf("Unknown command. Run 'epacmg' for help.\n");
    return 1;
}