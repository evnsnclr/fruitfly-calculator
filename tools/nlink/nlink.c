/*
 * nlink — minimal host-side link tool for the TI-Nspire CX II.
 * Thin CLI over libnspire (GPLv3, github.com/Vogtinator/libnspire).
 *
 *   nlink info
 *   nlink ls   <calc-dir>
 *   nlink put  <local-file> <calc-path>
 *   nlink get  <calc-path> <local-file>
 *   nlink rm   <calc-path>
 *   nlink mkdir <calc-dir>
 *   nlink shot <out.ppm>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nspire.h"

static int fail(const char *what, int rc) {
    fprintf(stderr, "nlink: %s failed: %s (%d)\n", what, nspire_strerror(rc), rc);
    return 1;
}

static int cmd_info(nspire_handle_t *h) {
    struct nspire_devinfo d;
    int rc = nspire_device_info(h, &d);
    if (rc) return fail("device_info", rc);
    printf("name:        %s\n", d.device_name);
    printf("hw_type:     0x%02x\n", d.hw_type);
    printf("os:          %u.%u.%u\n", d.versions[NSPIRE_VER_OS].major, d.versions[NSPIRE_VER_OS].minor, d.versions[NSPIRE_VER_OS].build);
    printf("boot1:       %u.%u.%u\n", d.versions[NSPIRE_VER_BOOT1].major, d.versions[NSPIRE_VER_BOOT1].minor, d.versions[NSPIRE_VER_BOOT1].build);
    printf("boot2:       %u.%u.%u\n", d.versions[NSPIRE_VER_BOOT2].major, d.versions[NSPIRE_VER_BOOT2].minor, d.versions[NSPIRE_VER_BOOT2].build);
    printf("storage:     %llu free / %llu total bytes\n", (unsigned long long)d.storage.free, (unsigned long long)d.storage.total);
    printf("ram:         %llu free / %llu total bytes\n", (unsigned long long)d.ram.free, (unsigned long long)d.ram.total);
    printf("battery:     status=0x%02x charging=%u\n", d.batt.status, d.batt.is_charging);
    printf("clock:       %u\n", d.clock_speed);
    printf("lcd:         %ux%u %ubpp mode=%u\n", d.lcd.width, d.lcd.height, d.lcd.bbp, d.lcd.sample_mode);
    printf("ext:         file=%s os=%s\n", d.extensions.file, d.extensions.os);
    printf("runlevel:    %d\n", d.runlevel);
    return 0;
}

static int cmd_ls(nspire_handle_t *h, const char *path) {
    struct nspire_dir_info *list;
    int rc = nspire_dirlist(h, path, &list);
    if (rc) return fail("dirlist", rc);
    for (unsigned long i = 0; i < list->num; i++) {
        struct nspire_dir_item *it = &list->items[i];
        time_t t = (time_t)it->date;
        char when[32];
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&t));
        printf("%c %10lu  %s  %s\n", it->type == NSPIRE_DIR ? 'd' : '-', it->size, when, it->name);
    }
    nspire_dirlist_free(list);
    return 0;
}

static int cmd_put(nspire_handle_t *h, const char *local, const char *remote) {
    FILE *f = fopen(local, "rb");
    if (!f) { perror(local); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *buf = malloc(n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); fclose(f); free(buf); return 1; }
    fclose(f);
    clock_t t0 = clock();
    time_t w0 = time(NULL);
    int rc = nspire_file_write(h, remote, buf, (size_t)n);
    free(buf);
    (void)t0;
    if (rc) return fail("file_write", rc);
    printf("sent %ld bytes -> %s (%lds)\n", n, remote, (long)(time(NULL) - w0));
    return 0;
}

static int cmd_get(nspire_handle_t *h, const char *remote, const char *local) {
    struct nspire_dir_item attr;
    int rc = nspire_attr(h, remote, &attr);
    if (rc) return fail("attr", rc);
    size_t cap = attr.size ? attr.size : 1, got = 0;
    void *buf = malloc(cap);
    if (!buf) return 1;
    rc = nspire_file_read(h, remote, buf, attr.size, &got);
    if (rc) { free(buf); return fail("file_read", rc); }
    FILE *f = fopen(local, "wb");
    if (!f) { perror(local); free(buf); return 1; }
    fwrite(buf, 1, got, f);
    fclose(f);
    free(buf);
    printf("received %zu bytes <- %s\n", got, remote);
    return 0;
}

static int cmd_shot(nspire_handle_t *h, const char *out) {
    struct nspire_image *img;
    int rc = nspire_screenshot(h, &img);
    if (rc) return fail("screenshot", rc);
    FILE *f = fopen(out, "wb");
    if (!f) { perror(out); free(img); return 1; }
    fprintf(f, "P6\n%u %u\n255\n", img->width, img->height);
    size_t px = (size_t)img->width * img->height;
    for (size_t i = 0; i < px; i++) {
        unsigned char rgb[3];
        if (img->bpp == 16) {
            unsigned v = img->data[2 * i] | (img->data[2 * i + 1] << 8);
            rgb[0] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
            rgb[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
            rgb[2] = (unsigned char)((v & 31) * 255 / 31);
        } else {
            rgb[0] = rgb[1] = rgb[2] = img->data[i];
        }
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("screenshot %ux%u %ubpp -> %s\n", img->width, img->height, img->bpp, out);
    free(img);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: nlink info | ls <dir> | put <local> <remote> | get <remote> <local> | rm <path> | mkdir <dir> | shot <out.ppm>\n");
        return 2;
    }
    nspire_handle_t *h;
    int rc = nspire_init(&h);
    if (rc) return fail("init (is the calculator connected and awake?)", rc);
    int ret = 2;
    const char *c = argv[1];
    if (!strcmp(c, "info")) ret = cmd_info(h);
    else if (!strcmp(c, "ls") && argc == 3) ret = cmd_ls(h, argv[2]);
    else if (!strcmp(c, "put") && argc == 4) ret = cmd_put(h, argv[2], argv[3]);
    else if (!strcmp(c, "get") && argc == 4) ret = cmd_get(h, argv[2], argv[3]);
    else if (!strcmp(c, "shot") && argc == 3) ret = cmd_shot(h, argv[2]);
    else if (!strcmp(c, "rm") && argc == 3) { rc = nspire_file_delete(h, argv[2]); ret = rc ? fail("delete", rc) : 0; }
    else if (!strcmp(c, "mkdir") && argc == 3) { rc = nspire_dir_create(h, argv[2]); ret = rc ? fail("mkdir", rc) : 0; }
    else fprintf(stderr, "nlink: bad command\n");
    nspire_free(h);
    return ret;
}
