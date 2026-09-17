#include "ramfs.h"
#include "../core/mem/memory.h"
#include "string.h"
#include "stdio.h"
#include "../core/initcall.h"
#include "../../limine.h"
#include "../drivers/serial/serial.h"
#include "iso9660.h"
#include "../drivers/display/psf2.h"

extern volatile struct limine_module_request module_request;

static int64_t ramfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    ramfs_file_data_t *fdata = (ramfs_file_data_t *)node->ptr;
    if (!fdata || offset >= node->length) return 0;

    uint64_t bytes_to_read = size;
    if (offset + size > node->length) {
        bytes_to_read = node->length - offset;
    }

    memcpy(buffer, fdata->buffer + offset, bytes_to_read);
    return bytes_to_read;
}

static int64_t ramfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, uint8_t *buffer) {
    ramfs_file_data_t *fdata = (ramfs_file_data_t *)node->ptr;
    if (!fdata) return -1;

    uint64_t required_size = offset + size;
    if (required_size > fdata->capacity) {
        size_t new_cap = required_size + 1024;
        uint8_t *new_buf = (uint8_t *)krealloc(fdata->buffer, new_cap);
        if (!new_buf) return -1;
        fdata->buffer = new_buf;
        fdata->capacity = new_cap;
    }

    memcpy(fdata->buffer + offset, buffer, size);
    if (required_size > node->length) {
        node->length = required_size;
    }
    return size;
}

static vfs_node_t *ramfs_readdir(vfs_node_t *node, uint32_t index) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;
    vfs_node_t *child = node->children;
    uint32_t i = 0;
    while (child) {
        if (i == index) return child;
        child = child->next;
        i++;
    }
    return NULL;
}

static vfs_node_t *ramfs_finddir(vfs_node_t *node, const char *name) {
    if (!(node->flags & FS_DIRECTORY)) return NULL;
    vfs_node_t *child = node->children;
    while (child) {
        if (strcmp(child->name, name) == 0) return child;
        child = child->next;
    }
    return NULL;
}

static vfs_node_t *ramfs_vfs_create(vfs_node_t *dir, const char *name, uint32_t flags) {
    if (flags & FS_DIRECTORY) {
        return ramfs_create_directory(dir, name);
    }
    return ramfs_create_file(dir, name, NULL, 0);
}

static vfs_file_operations_t ramfs_fops = {
    .read = ramfs_read,
    .write = ramfs_write,
    .open = NULL,
    .close = NULL,
    .readdir = ramfs_readdir,
    .finddir = ramfs_finddir,
    .create = ramfs_vfs_create
};

vfs_node_t *ramfs_create_root(void) {
    vfs_node_t *root = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    strcpy(root->name, "/");
    root->flags = FS_DIRECTORY;
    root->permissions = 0755;
    root->ops = &ramfs_fops;
    root->parent = root;
    return root;
}

vfs_node_t *ramfs_create_directory(vfs_node_t *parent, const char *name) {
    vfs_node_t *dir = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!dir) return NULL;

    strcpy(dir->name, name);
    dir->flags = FS_DIRECTORY;
    dir->permissions = 0755;
    dir->ops = &ramfs_fops;
    dir->parent = parent ? parent : dir;

    if (parent) {
        if (!parent->children) {
            parent->children = dir;
        } else {
            vfs_node_t *curr = parent->children;
            while (curr->next) curr = curr->next;
            curr->next = dir;
        }
    }
    return dir;
}

vfs_node_t *ramfs_create_file(vfs_node_t *parent, const char *name, void *data, size_t size) {
    vfs_node_t *file = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
    if (!file) return NULL;

    strcpy(file->name, name);
    file->flags = FS_FILE;
    file->permissions = 0644;
    file->length = size;
    file->ops = &ramfs_fops;
    file->parent = parent;

    ramfs_file_data_t *fdata = (ramfs_file_data_t *)kzalloc(sizeof(ramfs_file_data_t));
    if (!fdata) {
        kfree(file);
        return NULL;
    }

    if (size > 0 && data) {
        fdata->buffer = (uint8_t *)kmalloc(size);
        if (fdata->buffer) {
            memcpy(fdata->buffer, data, size);
            fdata->capacity = size;
        } else {
            kfree(fdata);
            kfree(file);
            return NULL;
        }
    }
    file->ptr = (vfs_node_t *)fdata;

    if (parent) {
        if (!parent->children) {
            parent->children = file;
        } else {
            vfs_node_t *curr = parent->children;
            while (curr->next) curr = curr->next;
            curr->next = file;
        }
    }
    return file;
}

static void ramfs_create_busybox_links(vfs_node_t *bin_dir, vfs_node_t *bbox_node) {
    if (!bin_dir || !bbox_node) return;

    static const char *applets[] = {
        "ls", "cat", "cp", "mv", "rm", "mkdir", "rmdir", "touch",
        "clear", "echo", "grep", "head", "tail", "wc", "uname",
        "date", "df", "free", "ps", "kill", "sleep", "chmod", "chown",
        "sh", NULL
    };

    for (int i = 0; applets[i] != NULL; i++) {
        if (!vfs_finddir(bin_dir, applets[i])) {
            vfs_node_t *link = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
            if (link) {
                memcpy(link, bbox_node, sizeof(vfs_node_t));
                strncpy(link->name, applets[i], sizeof(link->name) - 1);
                link->parent = bin_dir;
                link->next = bin_dir->children;
                bin_dir->children = link;
            }
        }
    }
}

static int __init ramfs_populate_modules_initcall(void) {
    if (!vfs_root) return 0;

    const char *standard_dirs[] = {
        "bin", "boot", "etc", "home", "proc", "root", "sys", "tmp", "drives", "cdrom", NULL
    };

    for (int i = 0; standard_dirs[i] != NULL; i++) {
        if (!vfs_finddir(vfs_root, standard_dirs[i])) {
            ramfs_create_directory(vfs_root, standard_dirs[i]);
        }
    }

    vfs_node_t *bin_dir = vfs_finddir(vfs_root, "bin");
    vfs_node_t *etc_dir = vfs_finddir(vfs_root, "etc");
    vfs_node_t *root_dir = vfs_finddir(vfs_root, "root");
    vfs_node_t *cdrom_dir = vfs_finddir(vfs_root, "cdrom");
    vfs_node_t *sys_dir = vfs_finddir(vfs_root, "sys");
    vfs_node_t *sys_bin_dir = sys_dir ? vfs_finddir(sys_dir, "bin") : NULL;
    if (sys_dir && !sys_bin_dir) {
        sys_bin_dir = ramfs_create_directory(sys_dir, "bin");
    }

    vfs_node_t *bbox_node = NULL;
    vfs_node_t *bashrc_node = NULL;

    vfs_node_t *iso_root = iso9660_mount_boot_drive();
    if (iso_root) {
        serial_puts(COM1, "[RAMFS] LiveCD media detected. Mounting /cdrom and populating /bin...\n");
        if (cdrom_dir) {
            cdrom_dir->flags |= FS_MOUNTPOINT;
            cdrom_dir->ptr = iso_root;
        }

        uint32_t idx = 0;
        vfs_node_t *entry = NULL;
        while ((entry = vfs_readdir(iso_root, idx++)) != NULL) {
            if (strstr(entry->name, "font.psf") || strstr(entry->name, ".psf")) {
                uint8_t *fbuf = (uint8_t *)kmalloc(entry->length);
                if (fbuf && vfs_read(entry, 0, entry->length, fbuf) == (int64_t)entry->length) {
                    psf2_init_default(fbuf, entry->length);
                    serial_puts(COM1, "[KERNEL] PSF2 Font loaded from ISO9660.\n");
                }
            }

            if (strcmp(entry->name, "bash.elf") == 0) {
                vfs_node_t *bsh = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                if (bsh && bin_dir) {
                    memcpy(bsh, entry, sizeof(vfs_node_t));
                    strcpy(bsh->name, "bash");
                    bsh->parent = bin_dir;
                    bsh->next = bin_dir->children;
                    bin_dir->children = bsh;
                }
            }

            if (strcmp(entry->name, ".bashrc") == 0 || strcmp(entry->name, "_bashrc") == 0) {
                bashrc_node = entry;
            }

            if (strcmp(entry->name, "BOOTX64.EFI") == 0 || strcmp(entry->name, "kernel.elf") == 0) {
                vfs_node_t *knode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                if (knode) {
                    memcpy(knode, entry, sizeof(vfs_node_t));
                    knode->parent = vfs_root;
                    knode->next = vfs_root->children;
                    vfs_root->children = knode;
                }
            }

            if (entry->flags & FS_FILE) {
                if (bin_dir && !vfs_finddir(bin_dir, entry->name)) {
                    vfs_node_t *bnode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                    if (bnode) {
                        memcpy(bnode, entry, sizeof(vfs_node_t));
                        bnode->parent = bin_dir;
                        bnode->next = bin_dir->children;
                        bin_dir->children = bnode;
                        if (strstr(entry->name, "busybox")) bbox_node = bnode;
                    }
                }
                if (sys_bin_dir && !vfs_finddir(sys_bin_dir, entry->name)) {
                    vfs_node_t *snode = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                    if (snode) {
                        memcpy(snode, entry, sizeof(vfs_node_t));
                        snode->parent = sys_bin_dir;
                        snode->next = sys_bin_dir->children;
                        sys_bin_dir->children = snode;
                    }
                }
            }
        }
    }

    if (bbox_node && bin_dir) {
        ramfs_create_busybox_links(bin_dir, bbox_node);
    }

    if (bashrc_node) {
        vfs_node_t *targets[] = { etc_dir, root_dir, vfs_root, NULL };
        for (int i = 0; targets[i] != NULL; i++) {
            if (!vfs_finddir(targets[i], ".bashrc")) {
                vfs_node_t *rc = (vfs_node_t *)kzalloc(sizeof(vfs_node_t));
                if (rc) {
                    memcpy(rc, bashrc_node, sizeof(vfs_node_t));
                    strcpy(rc->name, ".bashrc");
                    rc->parent = targets[i];
                    rc->next = targets[i]->children;
                    targets[i]->children = rc;
                }
            }
        }
    }

    return 0;
}
fs_initcall(ramfs_populate_modules_initcall);