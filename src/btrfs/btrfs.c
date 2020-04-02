/* Copyright (c) Mark Harmstone 2020
 *
 * This file is part of Quibble.
 *
 * Quibble is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * Quibble is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with Quibble.  If not, see <http://www.gnu.org/licenses/>. */

#include <efibind.h>
#include <efidef.h>
#include <efilink.h>
#include <stdbool.h>
#include <string.h>
#include "../misc.h"
#include "../quibbleproto.h"
#include "btrfs.h"

#define __S_IFDIR 0040000

EFI_SYSTEM_TABLE* systable;
EFI_BOOT_SERVICES* bs;

EFI_DRIVER_BINDING_PROTOCOL drvbind;

typedef struct {
    uint64_t address;
    LIST_ENTRY list_entry;
    CHUNK_ITEM chunk_item;
} chunk;

typedef struct {
    LIST_ENTRY list_entry;
    uint64_t id;
    ROOT_ITEM root_item;
    void* top_tree;
} root;

typedef struct {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL proto;
    EFI_QUIBBLE_PROTOCOL quibble_proto;
    EFI_OPEN_SUBVOL_PROTOCOL open_subvol_proto;
    superblock* sb;
    EFI_HANDLE controller;
    EFI_BLOCK_IO_PROTOCOL* block;
    EFI_DISK_IO_PROTOCOL* disk_io;
    bool chunks_loaded;
    LIST_ENTRY chunks;
    LIST_ENTRY roots;
    root* root_root;
    root* chunk_root;
    LIST_ENTRY list_entry;
    root* fsroot;
} volume;

typedef struct {
    void* data;
    KEY* key;
    void* item;
    uint16_t itemlen;
    uint16_t* positions;
} traverse_ptr;

typedef struct {
    EFI_FILE_PROTOCOL proto;
    root* r;
    uint64_t inode;
    volume* vol;
    bool inode_loaded;
    INODE_ITEM inode_item;
    uint64_t position;
    LIST_ENTRY* dir_position;
    WCHAR* name;
    LIST_ENTRY extents;
    LIST_ENTRY children;
    bool children_found;
} inode;

typedef struct {
    LIST_ENTRY list_entry;
    DIR_ITEM dir_item;
} inode_child;

typedef struct {
    LIST_ENTRY list_entry;
    uint64_t offset;
    EXTENT_DATA extent_data;
} extent;

typedef struct {
    LIST_ENTRY list_entry;
    char name[1];
} path_segment;

#define UNUSED(x) (void)(x)
#define sector_align(n, a) ((n)&((a)-1)?(((n)+(a))&~((a)-1)):(n))

#define COMPAT_FLAGS (BTRFS_INCOMPAT_FLAGS_MIXED_BACKREF | BTRFS_INCOMPAT_FLAGS_DEFAULT_SUBVOL | \
                     BTRFS_INCOMPAT_FLAGS_MIXED_GROUPS | BTRFS_INCOMPAT_FLAGS_BIG_METADATA | \
                     BTRFS_INCOMPAT_FLAGS_EXTENDED_IREF | BTRFS_INCOMPAT_FLAGS_SKINNY_METADATA | \
                     BTRFS_INCOMPAT_FLAGS_NO_HOLES)

__inline static void populate_file_handle(EFI_FILE_PROTOCOL* h);
static EFI_STATUS load_inode(inode* ino);

// crc32c.c
uint32_t calc_crc32c(uint32_t seed, uint8_t* msg, unsigned int msglen);

LIST_ENTRY volumes;

static EFI_STATUS drv_supported(EFI_DRIVER_BINDING_PROTOCOL* This, EFI_HANDLE ControllerHandle,
                                EFI_DEVICE_PATH_PROTOCOL* RemainingDevicePath) {
    EFI_STATUS Status;
    EFI_DISK_IO_PROTOCOL* disk_io;
    EFI_GUID guid_disk = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_GUID guid_block = EFI_BLOCK_IO_PROTOCOL_GUID;

    UNUSED(RemainingDevicePath);

    Status = bs->OpenProtocol(ControllerHandle, &guid_disk, (void**)&disk_io, This->DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_BY_DRIVER);

    if (EFI_ERROR(Status))
        return Status;

    bs->CloseProtocol(ControllerHandle, &guid_disk, This->DriverBindingHandle, ControllerHandle);

    return bs->OpenProtocol(ControllerHandle, &guid_block, NULL, This->DriverBindingHandle,
                            ControllerHandle, EFI_OPEN_PROTOCOL_TEST_PROTOCOL);
}

static EFI_STATUS bootstrap_roots(volume* vol) {
    EFI_STATUS Status;
    root* r;

    InitializeListHead(&vol->roots);

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(root), (void**)&r);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    memset(r, 0, sizeof(root));

    r->id = BTRFS_ROOT_ROOT;
    r->root_item.block_number = vol->sb->root_tree_addr;
    r->root_item.root_level = vol->sb->root_level;

    vol->root_root = r;

    InsertTailList(&vol->roots, &r->list_entry);

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(root), (void**)&r);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    memset(r, 0, sizeof(root));

    r->id = BTRFS_ROOT_CHUNK;
    r->root_item.block_number = vol->sb->chunk_tree_addr;
    r->root_item.root_level = vol->sb->chunk_root_level;

    vol->chunk_root = r;

    InsertTailList(&vol->roots, &r->list_entry);

    return EFI_SUCCESS;
}

static EFI_STATUS read_data(volume* vol, uint64_t address, uint32_t size, void* data) {
    EFI_STATUS Status;
    LIST_ENTRY* le;
    chunk* c = NULL;
    CHUNK_ITEM_STRIPE* stripes;

    le = vol->chunks.Flink;
    while (le != &vol->chunks) {
        chunk* c2 = _CR(le, chunk, list_entry);

        if (address >= c2->address && address < c2->address + c2->chunk_item.size) {
            c = c2;
            break;
        } else if (c2->address > address)
            break;

        le = le->Flink;
    }

    if (!c) {
        print(L"Could not find chunk for address ");
        print_hex(address);
        print(L".\r\n");
        return EFI_INVALID_PARAMETER;
    }

    // FIXME - support RAID

    if (c->chunk_item.type & BLOCK_FLAG_RAID0) {
        print(L"FIXME - support RAID0.\r\n");
        return EFI_INVALID_PARAMETER;
    } else if (c->chunk_item.type & BLOCK_FLAG_RAID10) {
        print(L"FIXME - support RAID10.\r\n");
        return EFI_INVALID_PARAMETER;
    } else if (c->chunk_item.type & BLOCK_FLAG_RAID5) {
        print(L"FIXME - support RAID5.\r\n");
        return EFI_INVALID_PARAMETER;
    } else if (c->chunk_item.type & BLOCK_FLAG_RAID6) {
        print(L"FIXME - support RAID6.\r\n");
        return EFI_INVALID_PARAMETER;
    }

    stripes = (CHUNK_ITEM_STRIPE*)((uint8_t*)&c->chunk_item + sizeof(CHUNK_ITEM));

    for (unsigned int i = 0; i < c->chunk_item.num_stripes; i++) {
        // FIXME - support other devices
        // FIXME - use other stripe if csum error

        if (stripes[i].dev_id == vol->sb->dev_item.dev_id) {
            Status = vol->block->ReadBlocks(vol->block, vol->block->Media->MediaId,
                                            (stripes[i].offset + address - c->address) / vol->block->Media->BlockSize,
                                            size, data);
            if (EFI_ERROR(Status)) {
                print_error(L"ReadBlocks", Status);
                continue;
            }

            return EFI_SUCCESS;
        }
    }

    return EFI_VOLUME_CORRUPTED;
}

static int keycmp(KEY* key1, KEY* key2) {
    if (key1->obj_id < key2->obj_id)
        return -1;

    if (key1->obj_id > key2->obj_id)
        return 1;

    if (key1->obj_type < key2->obj_type)
        return -1;

    if (key1->obj_type > key2->obj_type)
        return 1;

    if (key1->offset < key2->offset)
        return -1;

    if (key1->offset > key2->offset)
        return 1;

    return 0;
}

static EFI_STATUS find_item(volume* vol, root* r, traverse_ptr* tp, KEY* searchkey) {
    EFI_STATUS Status;
    tree_header* tree;
    uint64_t addr;

    Status = bs->AllocatePool(EfiBootServicesData, (r->root_item.root_level + 1) * vol->sb->leaf_size, (void**)&tp->data);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    Status = bs->AllocatePool(EfiBootServicesData, (r->root_item.root_level + 1) * sizeof(uint16_t), (void**)&tp->positions);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        bs->FreePool(tp->data);
        return Status;
    }

    addr = r->root_item.block_number;

    if (!r->top_tree) {
        Status = bs->AllocatePool(EfiBootServicesData, vol->sb->leaf_size, &r->top_tree);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return Status;
        }

        Status = read_data(vol, r->root_item.block_number, vol->sb->leaf_size, r->top_tree);
        if (EFI_ERROR(Status)) {
            print_error(L"read_data", Status);
            bs->FreePool(r->top_tree);
            r->top_tree = NULL;
            return Status;
        }
    }

    memcpy(tp->data, r->top_tree, vol->sb->leaf_size);

    for (unsigned int i = 0; i < (unsigned int)(r->root_item.root_level + 1); i++) {
        if (i != 0) {
            Status = read_data(vol, addr, vol->sb->leaf_size, (uint8_t*)tp->data + (i * vol->sb->leaf_size));
            if (EFI_ERROR(Status)) {
                print_error(L"read_data", Status);
                return Status;
            }
        }

        tree = (tree_header*)((uint8_t*)tp->data + (i * vol->sb->leaf_size));

        // FIXME - check csum

        if (tree->level != r->root_item.root_level - i) {
            print(L"Tree level was ");
            print_dec(tree->level);
            print(L", expected ");
            print_dec(r->root_item.root_level - i);
            print(L".\r\n");
            return EFI_VOLUME_CORRUPTED;
        }

        if (tree->level != 0) {
            internal_node* nodes = (internal_node*)((uint8_t*)tree + sizeof(tree_header));

            for (unsigned int j = 0; j < tree->num_items; j++) {
                int cmp = keycmp(searchkey, &nodes[j].key);

                if (cmp == 0 || (cmp != -1 && j == tree->num_items - 1) || (cmp == -1 && j == 0)) {
                    tp->positions[i] = j;
                    addr = nodes[j].address;
                    break;
                }

                if (cmp == -1) {
                    tp->positions[i] = j - 1;
                    addr = nodes[j - 1].address;
                    break;
                }
            }
        } else {
            leaf_node* nodes = (leaf_node*)((uint8_t*)tree + sizeof(tree_header));

            for (unsigned int j = 0; j < tree->num_items; j++) {
                int cmp = keycmp(searchkey, &nodes[j].key);

                if (cmp == 0 || (cmp == -1 && j == 0)) {
                    tp->key = &nodes[j].key;
                    tp->item = (uint8_t*)nodes + nodes[j].offset;
                    tp->itemlen = nodes[j].size;
                    tp->positions[i] = j;
                    return EFI_SUCCESS;
                }

                if (cmp == -1) {
                    tp->key = &nodes[j - 1].key;
                    tp->item = (uint8_t*)nodes + nodes[j - 1].offset;
                    tp->itemlen = nodes[j - 1].size;
                    tp->positions[i] = j - 1;
                    return EFI_SUCCESS;
                }
            }

            tp->key = &nodes[tree->num_items - 1].key;
            tp->item = (uint8_t*)nodes + nodes[tree->num_items - 1].offset;
            tp->itemlen = nodes[tree->num_items - 1].size;
            tp->positions[i] = tree->num_items - 1;

            return EFI_SUCCESS;
        }
    }

    return EFI_NOT_FOUND;
}

static EFI_STATUS next_item(volume* vol, traverse_ptr* tp) {
    EFI_STATUS Status;
    uint8_t level = ((tree_header*)tp->data)->level;

    tp->positions[level]++;

    for (int i = level; i >= 0; i--) {
        tree_header* tree = (tree_header*)((uint8_t*)tp->data + (i * vol->sb->leaf_size));

        if (tp->positions[i] == tree->num_items) {
            if (i == 0)
                return EFI_NOT_FOUND;

            tp->positions[i-1]++;
        } else {
            leaf_node* nodes;

            for (unsigned int j = i + 1; j <= level; j++) {
                internal_node* int_nodes = (internal_node*)((uint8_t*)tp->data + ((j - 1) * vol->sb->leaf_size) + sizeof(tree_header));
                uint64_t addr = int_nodes[tp->positions[j - 1]].address;

                Status = read_data(vol, addr, vol->sb->leaf_size, (uint8_t*)tp->data + (j * vol->sb->leaf_size));
                if (EFI_ERROR(Status)) {
                    print_error(L"read_data", Status);
                    return Status;
                }

                // FIXME - check crc32

                tp->positions[j] = 0;
            }

            nodes = (leaf_node*)((uint8_t*)tp->data + (level * vol->sb->leaf_size) + sizeof(tree_header));

            tp->key = &nodes[tp->positions[level]].key;
            tp->item = (uint8_t*)nodes + nodes[tp->positions[level]].offset;
            tp->itemlen = nodes[tp->positions[level]].size;

            return EFI_SUCCESS;
        }
    }

    return EFI_SUCCESS;
}

static void free_traverse_ptr(traverse_ptr* tp) {
    bs->FreePool(tp->data);
}

static EFI_STATUS load_roots(volume* vol) {
    EFI_STATUS Status;
    traverse_ptr tp;
    KEY searchkey;

    searchkey.obj_id = 0;
    searchkey.obj_type = 0;
    searchkey.offset = 0;

    Status = find_item(vol, vol->root_root, &tp, &searchkey);
    if (EFI_ERROR(Status)) {
        print_error(L"find_item", Status);
        return Status;
    }

    do {
        if (tp.key->obj_type == TYPE_ROOT_ITEM && tp.itemlen >= sizeof(ROOT_ITEM)) {
            root* r;

            Status = bs->AllocatePool(EfiBootServicesData, sizeof(root), (void**)&r);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                return Status;
            }

            memset(r, 0, sizeof(root));

            r->id = tp.key->obj_id;
            memcpy(&r->root_item, tp.item, sizeof(ROOT_ITEM));

            if (r->id > _CR(vol->roots.Blink, root, list_entry)->id) {
                InsertTailList(&vol->roots, &r->list_entry);
            } else {
                LIST_ENTRY* le = vol->roots.Flink;
                bool inserted = false;

                while (le != &vol->roots) {
                    root* r2 = _CR(le, root, list_entry);

                    if (r2->id > r->id) {
                        InsertHeadList(r2->list_entry.Blink, &r->list_entry);
                        inserted = true;
                        break;
                    }

                    le = le->Flink;
                }

                if (!inserted)
                    InsertTailList(&vol->roots, &r->list_entry);
            }
        }

        Status = next_item(vol, &tp);
        if (Status == EFI_NOT_FOUND)
            break;
        else if (EFI_ERROR(Status)) {
            print_error(L"next_item", Status);
            break;
        }
    } while (true);

    free_traverse_ptr(&tp);

    return EFI_SUCCESS;
}

static EFI_STATUS find_default_subvol(volume* vol, uint64_t* subvol) {
    EFI_STATUS Status;
    KEY searchkey;
    traverse_ptr tp;
    DIR_ITEM* di;

    static const char fn[] = "default";
    static uint32_t crc32 = 0x8dbfc2d2;

    // get default subvol

    searchkey.obj_id = vol->sb->root_dir_objectid;
    searchkey.obj_type = TYPE_DIR_ITEM;
    searchkey.offset = crc32;

    Status = find_item(vol, vol->root_root, &tp, &searchkey);
    if (EFI_ERROR(Status)) {
        print_error(L"find_item", Status);
        return Status;
    }

    if (keycmp(tp.key, &searchkey)) {
        print(L"Could not find (");
        print_hex(searchkey.obj_id);
        print(L",");
        print_hex(searchkey.obj_type);
        print(L",");
        print_hex(searchkey.offset);
        print(L") in root tree.\r\n");

        Status = EFI_NOT_FOUND;
        goto end;
    }

    if (tp.itemlen < sizeof(DIR_ITEM)) {
        print(L"(");
        print_hex(searchkey.obj_id);
        print(L",");
        print_hex(searchkey.obj_type);
        print(L",");
        print_hex(searchkey.offset);
        print(L") was ");
        print_dec(tp.itemlen);
        print(L" bytes, expected at least ");
        print_dec(sizeof(DIR_ITEM));
        print(L".\r\n");

        Status = EFI_NOT_FOUND;
        goto end;
    }

    di = (DIR_ITEM*)tp.item;

    if (tp.itemlen < offsetof(DIR_ITEM, name[0]) + di->n) {
        print(L"(");
        print_hex(searchkey.obj_id);
        print(L",");
        print_hex(searchkey.obj_type);
        print(L",");
        print_hex(searchkey.offset);
        print(L") was ");
        print_dec(tp.itemlen);
        print(L" bytes, expected ");
        print_dec(offsetof(DIR_ITEM, name[0]) + di->n);
        print(L".\r\n");

        Status = EFI_NOT_FOUND;
        goto end;
    }

    if (di->n != sizeof(fn) - 1 || memcmp(di->name, fn, di->n)) {
        print(L"root DIR_ITEM had same CRC32, but was not \"default\"\r\n");

        Status = EFI_NOT_FOUND;
        goto end;
    }

    if (di->key.obj_type != TYPE_ROOT_ITEM) {
        print(L"default root has key (");
        print_hex(di->key.obj_id);
        print(L",");
        print_hex(di->key.obj_type);
        print(L",");
        print_hex(di->key.offset);
        print(L"), expected subvolume\r\n");

        Status = EFI_NOT_FOUND;
        goto end;
    }

    *subvol = di->key.obj_id;

    Status = EFI_SUCCESS;

end:
    free_traverse_ptr(&tp);

    return Status;
}

static EFI_STATUS load_chunks(volume* vol) {
    EFI_STATUS Status;
    uint8_t* data;
    uint32_t n = vol->sb->n;
    KEY searchkey;
    traverse_ptr tp;
    LIST_ENTRY chunks2;
    LIST_ENTRY* le;
    uint64_t subvol_no = BTRFS_ROOT_FSTREE;

    InitializeListHead(&vol->chunks);

    // load bootstrapped chunks

    data = (uint8_t*)vol->sb->sys_chunk_array;

    while (n >= sizeof(KEY) + sizeof(CHUNK_ITEM) + sizeof(CHUNK_ITEM_STRIPE)) {
        KEY* key = (KEY*)data;
        CHUNK_ITEM* ci;
        chunk* c;

        if (key->obj_type != TYPE_CHUNK_ITEM)
            break;

        n -= sizeof(KEY);
        data += sizeof(KEY);

        ci = (CHUNK_ITEM*)data;

        if (n < sizeof(CHUNK_ITEM) + (ci->num_stripes * sizeof(CHUNK_ITEM_STRIPE)))
            break;

        Status = bs->AllocatePool(EfiBootServicesData,
                                  offsetof(chunk, chunk_item) + sizeof(CHUNK_ITEM) + (ci->num_stripes * sizeof(CHUNK_ITEM_STRIPE)),
                                  (void**)&c);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return Status;
        }

        c->address = key->offset;
        memcpy(&c->chunk_item, data, sizeof(CHUNK_ITEM) + (ci->num_stripes * sizeof(CHUNK_ITEM_STRIPE)));
        InsertTailList(&vol->chunks, &c->list_entry);

        data += sizeof(CHUNK_ITEM) + (ci->num_stripes * sizeof(CHUNK_ITEM_STRIPE));
        n -= sizeof(CHUNK_ITEM) + (ci->num_stripes * sizeof(CHUNK_ITEM_STRIPE));
    }

    Status = bootstrap_roots(vol);
    if (EFI_ERROR(Status)) {
        print_error(L"bootstrap_roots", Status);
        return Status;
    }

    InitializeListHead(&chunks2);

    searchkey.obj_id = 0;
    searchkey.obj_type = 0;
    searchkey.offset = 0;

    Status = find_item(vol, vol->chunk_root, &tp, &searchkey);
    if (EFI_ERROR(Status)) {
        print_error(L"find_item", Status);
        return Status;
    }

    do {
        if (tp.key->obj_type == TYPE_CHUNK_ITEM && tp.itemlen >= sizeof(CHUNK_ITEM)) {
            chunk* c;
            CHUNK_ITEM* ci;

            ci = (CHUNK_ITEM*)tp.item;

            if (tp.itemlen >= sizeof(CHUNK_ITEM) + (ci->num_stripes * sizeof(CHUNK_ITEM_STRIPE))) {
                Status = bs->AllocatePool(EfiBootServicesData, offsetof(chunk, chunk_item) + tp.itemlen, (void**)&c);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePool", Status);
                    return Status;
                }

                c->address = tp.key->offset;
                memcpy(&c->chunk_item, tp.item, tp.itemlen);
                InsertTailList(&chunks2, &c->list_entry);
            }
        }

        Status = next_item(vol, &tp);
        if (Status == EFI_NOT_FOUND)
            break;
        else if (EFI_ERROR(Status)) {
            print_error(L"next_item", Status);
            break;
        }
    } while (true);

    free_traverse_ptr(&tp);

    // replace chunks
    while (!IsListEmpty(&vol->chunks)) {
        chunk* c = _CR(vol->chunks.Flink, chunk, list_entry);

        RemoveEntryList(&c->list_entry);
        bs->FreePool(c);
    }

    vol->chunks.Flink = chunks2.Flink;
    vol->chunks.Flink->Blink = &vol->chunks;
    vol->chunks.Blink = chunks2.Blink;
    vol->chunks.Blink->Flink = &vol->chunks;

    Status = load_roots(vol);
    if (EFI_ERROR(Status)) {
        print_error(L"load_roots", Status);
        return Status;
    }

    if (vol->sb->incompat_flags & BTRFS_INCOMPAT_FLAGS_DEFAULT_SUBVOL) {
        Status = find_default_subvol(vol, &subvol_no);
        if (EFI_ERROR(Status))
            return Status;
    }

    le = vol->roots.Flink;
    while (le != &vol->roots) {
        root* r2 = _CR(le, root, list_entry);

        if (r2->id == subvol_no) {
            vol->fsroot = r2;
            break;
        }

        le = le->Flink;
    }

    vol->chunks_loaded = true;

    return EFI_SUCCESS;
}

static EFI_STATUS find_file_in_dir(volume* vol, root* r, uint64_t inode_num, WCHAR* name, unsigned int name_len,
                                   root** out_r, uint64_t* out_inode) {
    EFI_STATUS Status;
    unsigned int fnlen;
    char* fn;
    uint32_t hash;
    KEY searchkey;
    traverse_ptr tp;
    DIR_ITEM* di;
    unsigned int len;

    // convert name from UTF-16 to UTF-8

    Status = utf16_to_utf8(NULL, 0, &fnlen, name, name_len * sizeof(WCHAR));
    if (EFI_ERROR(Status)) {
        print_error(L"utf16_to_utf8", Status);
        return Status;
    }

    Status = bs->AllocatePool(EfiBootServicesData, fnlen, (void**)&fn);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    Status = utf16_to_utf8(fn, fnlen, &fnlen, name, name_len * sizeof(WCHAR));
    if (EFI_ERROR(Status)) {
        print_error(L"utf16_to_utf8", Status);
        bs->FreePool(fn);
        return Status;
    }

    // get CRC32 hash of name

    hash = calc_crc32c(0xfffffffe, (uint8_t*)fn, fnlen);

    // lookup DIR_ITEM of hash

    searchkey.obj_id = inode_num;
    searchkey.obj_type = TYPE_DIR_ITEM;
    searchkey.offset = hash;

    Status = find_item(vol, r, &tp, &searchkey);
    if (Status == EFI_NOT_FOUND) {
        bs->FreePool(fn);
        return Status;
    } else if (EFI_ERROR(Status)) {
        print_error(L"find_item", Status);
        bs->FreePool(fn);
        return Status;
    }

    if (keycmp(tp.key, &searchkey)) {
        bs->FreePool(fn);
        free_traverse_ptr(&tp);
        return EFI_NOT_FOUND;
    }

    di = (DIR_ITEM*)tp.item;
    len = tp.itemlen;

    while (len >= sizeof(DIR_ITEM) && len >= offsetof(DIR_ITEM, name[0]) + di->m + di->n) {
        if (di->n == fnlen && !memcmp(fn, di->name, fnlen)) {
            if (di->key.obj_type == TYPE_ROOT_ITEM) {
                LIST_ENTRY* le = vol->roots.Flink;

                *out_r = NULL;
                *out_inode = SUBVOL_ROOT_INODE;

                while (le != &vol->roots) {
                    root* r = _CR(le, root, list_entry);

                    if (r->id == di->key.obj_id)
                        *out_r = r;
                    else if (r->id > di->key.obj_id)
                        break;

                    le = le->Flink;
                }

                if (!*out_r) {
                    print(L"Could not find subvol ");
                    print_hex(di->key.obj_id);
                    print(L".\r\n");

                    bs->FreePool(fn);
                    free_traverse_ptr(&tp);

                    return EFI_NOT_FOUND;
                }
            } else {
                *out_r = r;
                *out_inode = di->key.obj_id;
            }

            bs->FreePool(fn);
            free_traverse_ptr(&tp);
            return EFI_SUCCESS;
        }

        len -= offsetof(DIR_ITEM, name[0]) + di->m + di->n;
        di = (DIR_ITEM*)((uint8_t*)di + offsetof(DIR_ITEM, name[0]) + di->m + di->n);
    }

    bs->FreePool(fn);

    free_traverse_ptr(&tp);

    return EFI_NOT_FOUND;
}

static EFI_STATUS find_file_in_dir_cached(volume* vol, inode* ino, WCHAR* name, unsigned int name_len,
                                          root** out_r, uint64_t* out_inode) {
    EFI_STATUS Status;
    unsigned int fnlen;
    char* fn;
    LIST_ENTRY* le;

    // convert name from UTF-16 to UTF-8

    Status = utf16_to_utf8(NULL, 0, &fnlen, name, name_len * sizeof(WCHAR));
    if (EFI_ERROR(Status)) {
        print_error(L"utf16_to_utf8", Status);
        return Status;
    }

    Status = bs->AllocatePool(EfiBootServicesData, fnlen, (void**)&fn);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    Status = utf16_to_utf8(fn, fnlen, &fnlen, name, name_len * sizeof(WCHAR));
    if (EFI_ERROR(Status)) {
        print_error(L"utf16_to_utf8", Status);
        bs->FreePool(fn);
        return Status;
    }

    le = ino->children.Flink;

    while (le != &ino->children) {
        DIR_ITEM* di = &(_CR(le, inode_child, list_entry)->dir_item);

        if (di->n == fnlen && !memcmp(fn, di->name, fnlen)) {
            if (di->key.obj_type == TYPE_ROOT_ITEM) {
                LIST_ENTRY* le = vol->roots.Flink;

                *out_r = NULL;
                *out_inode = SUBVOL_ROOT_INODE;

                while (le != &vol->roots) {
                    root* r = _CR(le, root, list_entry);

                    if (r->id == di->key.obj_id)
                        *out_r = r;
                    else if (r->id > di->key.obj_id)
                        break;

                    le = le->Flink;
                }

                if (!*out_r) {
                    print(L"Could not find subvol ");
                    print_hex(di->key.obj_id);
                    print(L".\r\n");

                    bs->FreePool(fn);

                    return EFI_NOT_FOUND;
                }
            } else {
                *out_r = ino->r;
                *out_inode = di->key.obj_id;
            }

            bs->FreePool(fn);

            return EFI_SUCCESS;
        }

        le = le->Flink;
    }

    bs->FreePool(fn);

    return EFI_NOT_FOUND;
}

static void normalize_path(WCHAR* path) {
    size_t len = wcslen(path);

    for (unsigned int i = 1; i < len; i++) {
        if (path[i] == '\\' && path[i-1] == '\\') {
            // remove empty directory name
            memcpy(&path[i], &path[i+1], (len - i) * sizeof(WCHAR));
            len--;
            i--;
            continue;
        } else if (path[i] == '.' && path[i-1] == '\\' && (path[i+1] == 0 || path[i+1] == '\\')) {
            // remove .
            if (path[i+1] == '\\') {
                memcpy(&path[i], &path[i+2], (len - i - 1) * sizeof(WCHAR));
                len -= 2;
                i--;
                continue;
            } else if (path[i+1] == 0) {
                path[i] = 0;
                return;
            }
        } else if (i >= 3 && path[i] == '.' && path[i-1] == '.' && path[i-2] == '\\' && (path[i+1] == 0 || path[i+1] == '\\')) {
            unsigned int bs = 0;

            // remove ..

            for (int j = i - 3; j >= 0; j--) {
                if (path[j] == '\\') {
                    bs = j;
                    break;
                }
            }

            if (path[i+1] == '\\') {
                memcpy(&path[bs + 1], &path[i + 2], (len - i - 1) * sizeof(WCHAR));
                len -= i - bs + 1;
                i = bs;
                continue;
            } else {
                path[bs] = 0;
                return;
            }
        }
    }
}

static EFI_STATUS find_children(inode* ino) {
    EFI_STATUS Status;
    KEY searchkey;
    traverse_ptr tp;

    searchkey.obj_id = ino->inode;
    searchkey.obj_type = TYPE_DIR_INDEX;
    searchkey.offset = ino->position;

    Status = find_item(ino->vol, ino->r, &tp, &searchkey);
    if (EFI_ERROR(Status)) {
        print_error(L"find_item", Status);
        return Status;
    }

    while (tp.key->obj_id < ino->inode || (tp.key->obj_id == ino->inode && tp.key->obj_type < TYPE_DIR_INDEX)) {
        Status = next_item(ino->vol, &tp);

        if (Status == EFI_NOT_FOUND) { // no children
            ino->children_found = true;
            free_traverse_ptr(&tp);
            return EFI_SUCCESS;
        } else if (EFI_ERROR(Status)) {
            print_error(L"next_item", Status);
            free_traverse_ptr(&tp);
            return Status;
        }
    }

    while (tp.key->obj_id == ino->inode && tp.key->obj_type == TYPE_DIR_INDEX) {
        DIR_ITEM* di = (DIR_ITEM*)tp.item;

        if (tp.itemlen < sizeof(DIR_ITEM)) {
            print(L"DIR_ITEM length was ");
            print_dec(tp.itemlen);
            print(L" bytes, expected at least ");
            print_dec(sizeof(DIR_ITEM));
            print(L".\r\n");
        } else if (tp.itemlen < offsetof(DIR_ITEM, name[0]) + di->m + di->n) {
            print(L"DIR_ITEM length was ");
            print_dec(tp.itemlen);
            print(L" bytes, expected ");
            print_dec(offsetof(DIR_ITEM, name[0]) + di->m + di->n);
            print(L".\r\n");
        } else {
            inode_child* ic;

            Status = bs->AllocatePool(EfiBootServicesData, offsetof(inode_child, dir_item) + tp.itemlen, (void**)&ic);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                free_traverse_ptr(&tp);
                return Status;
            }

            memcpy(&ic->dir_item, tp.item, tp.itemlen);
            InsertTailList(&ino->children, &ic->list_entry);
        }

        Status = next_item(ino->vol, &tp);

        if (Status == EFI_NOT_FOUND)
            break;
        else if (EFI_ERROR(Status)) {
            print_error(L"next_item", Status);
            free_traverse_ptr(&tp);
            return Status;
        }
    }

    ino->children_found = true;
    ino->dir_position = ino->children.Flink;

    free_traverse_ptr(&tp);

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_open(struct _EFI_FILE_HANDLE* File, struct _EFI_FILE_HANDLE** NewHandle, CHAR16* FileName,
                                   UINT64 OpenMode, UINT64 Attributes) {
    EFI_STATUS Status;
    inode* ino = _CR(File, inode, proto);
    WCHAR* fn = FileName;
    root* r;
    uint64_t inode_num;
    inode* ino2;
    WCHAR* path;
    size_t pathlen;
    size_t ino_name_len = ino->name ? wcslen(ino->name) : 0;

    UNUSED(Attributes);

    if (OpenMode & EFI_FILE_MODE_CREATE)
        return EFI_UNSUPPORTED;

    if (fn[0] == '\\') {
        pathlen = wcslen(fn);

        Status = bs->AllocatePool(EfiBootServicesData, (pathlen + 1) * sizeof(WCHAR), (void**)&path);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return Status;
        }

        memcpy(path, fn, pathlen * sizeof(WCHAR));
        path[pathlen] = 0;
    } else {
        WCHAR* p;

        pathlen = wcslen(fn) + 1 + ino_name_len;

        Status = bs->AllocatePool(EfiBootServicesData, (pathlen + 1) * sizeof(WCHAR), (void**)&path);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);
            return Status;
        }

        if (ino->name) {
            memcpy(path, ino->name, ino_name_len * sizeof(WCHAR));
            p = &path[ino_name_len];

            *p = '\\';
            p++;
        } else {
            path[0] = '\\';
            p = &path[1];
        }

        memcpy(p, fn, wcslen(fn) * sizeof(WCHAR));
        p += wcslen(fn);

        *p = 0;
    }

    normalize_path(path);

    if (path[0] != 0 && path[1] != 0 && path[wcslen(path) - 1] == '\\')
        path[wcslen(path) - 1] = 0;

    if (path[0] == 0) {
        path[0] = '\\';
        path[1] = 0;
    }

    pathlen = wcslen(path);

    if (ino->name && pathlen > ino_name_len && !memcmp(ino->name, path, ino_name_len * sizeof(WCHAR)) && path[ino_name_len] == '\\') {
        r = ino->r;
        inode_num = ino->inode;
        fn = &path[ino_name_len + 1];
    } else {
        r = ino->vol->fsroot;
        inode_num = SUBVOL_ROOT_INODE;
        fn = &path[1];
    }

    // FIXME - follow symlinks?

    while (true) {
        unsigned int backslash;

        if (fn[0] == 0)
            break;

        {
            unsigned int i = 0;

            while (fn[i] != '\\' && fn[i] != 0) {
                i++;
            }

            backslash = i;
        }

        if (backslash == 0) {
            fn++;
            continue;
        } else if (backslash == 1 && fn[0] == '.') {
            if (fn[1] == 0)
                break;

            fn += 2;
            continue;
        } else if (backslash == 2 && fn[0] == '.' && fn[1] == '.') {
            // shouldn't happen - removed by normalize_path
            return EFI_INVALID_PARAMETER;
        } else {
            if (r == ino->r && inode_num == ino->inode) {
                if (!ino->children_found) {
                    Status = find_children(ino);
                    if (EFI_ERROR(Status)) {
                        print_error(L"find_children", Status);
                        bs->FreePool(path);
                        return Status;
                    }
                }

                Status = find_file_in_dir_cached(ino->vol, ino, fn, backslash, &r, &inode_num);
                if (Status == EFI_NOT_FOUND) {
                    bs->FreePool(path);
                    return Status;
                } else if (EFI_ERROR(Status)) {
                    print_error(L"find_file_in_dir_cached", Status);
                    bs->FreePool(path);
                    return Status;
                }
            } else {
                Status = find_file_in_dir(ino->vol, r, inode_num, fn, backslash, &r, &inode_num);
                if (Status == EFI_NOT_FOUND) {
                    bs->FreePool(path);
                    return Status;
                } else if (EFI_ERROR(Status)) {
                    print_error(L"find_file_in_dir", Status);
                    bs->FreePool(path);
                    return Status;
                }
            }

            fn += backslash;

            if (fn[0] == '\\')
                fn++;
        }
    }

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(inode), (void**)&ino2);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        bs->FreePool(path);
        return Status;
    }

    memset(ino2, 0, sizeof(inode));

    populate_file_handle(&ino2->proto);

    InitializeListHead(&ino2->children);

    ino2->r = r;
    ino2->inode = inode_num;
    ino2->vol = ino->vol;
    ino2->name = path;

    *NewHandle = &ino2->proto;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_close(struct _EFI_FILE_HANDLE* File) {
    inode* ino = _CR(File, inode, proto);

    while (!IsListEmpty(&ino->children)) {
        inode_child* ic = _CR(ino->children.Flink, inode_child, list_entry);

        RemoveEntryList(&ic->list_entry);
        bs->FreePool(ic);
    }

    if (ino->name)
        bs->FreePool(ino->name);

    if (ino->inode_loaded) {
        while (!IsListEmpty(&ino->extents)) {
            extent* ext = _CR(ino->extents.Flink, extent, list_entry);

            RemoveEntryList(&ext->list_entry);
            bs->FreePool(ext);
        }
    }

    bs->FreePool(ino);

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_delete(struct _EFI_FILE_HANDLE* File) {
    UNUSED(File);

    return EFI_UNSUPPORTED;
}

static EFI_STATUS read_dir(inode* ino, UINTN* bufsize, void* buf) {
    EFI_STATUS Status;
    DIR_ITEM* di;
    unsigned int fnlen;
    EFI_FILE_INFO* info;

    if (!ino->children_found) {
        Status = find_children(ino);
        if (EFI_ERROR(Status)) {
            print_error(L"find_children", Status);
            return Status;
        }
    }

    // no more entries
    if (ino->dir_position == &ino->children) {
        *bufsize = 0;
        return EFI_SUCCESS;
    }

    di = &(_CR(ino->dir_position, inode_child, list_entry)->dir_item);

    Status = utf8_to_utf16(NULL, 0, &fnlen, di->name, di->n);
    if (EFI_ERROR(Status)) {
        print_error(L"utf8_to_utf16", Status);
        return Status;
    }

    if (*bufsize < offsetof(EFI_FILE_INFO, FileName[0]) + fnlen) {
        *bufsize = offsetof(EFI_FILE_INFO, FileName[0]) + fnlen;
        return EFI_BUFFER_TOO_SMALL;
    }

    *bufsize = offsetof(EFI_FILE_INFO, FileName[0]) + fnlen;
    info = (EFI_FILE_INFO*)buf;

    info->Size = offsetof(EFI_FILE_INFO, FileName[0]) + fnlen;
    //info->FileSize = ino->inode_item.st_size; // FIXME
    //info->PhysicalSize = ino->inode_item.st_blocks; // FIXME
//         info->CreateTime; // FIXME
//         info->LastAccessTime; // FIXME
//         info->ModificationTime; // FIXME
    info->Attribute = di->type == BTRFS_TYPE_DIRECTORY ? EFI_FILE_DIRECTORY : 0;

    Status = utf8_to_utf16(info->FileName, fnlen, &fnlen, di->name, di->n);
    if (EFI_ERROR(Status)) {
        print_error(L"utf8_to_utf16", Status);
        return Status;
    }

    info->FileName[fnlen / sizeof(WCHAR)] = 0;

    ino->position++;
    ino->dir_position = ino->dir_position->Flink;

    return EFI_SUCCESS;
}

static EFI_STATUS read_file(inode* ino, UINTN* bufsize, void* buf) {
    EFI_STATUS Status;
    unsigned int to_read, left;
    uint64_t pos;
    uint8_t* dest;
    LIST_ENTRY* le;

    if (!ino->inode_loaded) {
        Status = load_inode(ino);
        if (EFI_ERROR(Status)) {
            print_error(L"load_inode", Status);
            return Status;
        }
    }

    // FIXME - check is actually file (check st_mode)

    if (ino->position >= ino->inode_item.st_size) { // past end of file
        *bufsize = 0;
        return EFI_SUCCESS;
    }

    to_read = *bufsize;

    if (ino->position + to_read >= ino->inode_item.st_size)
        to_read = ino->inode_item.st_size - ino->position;

    dest = (uint8_t*)buf;
    left = to_read;
    pos = ino->position;

    memset(dest, 0, to_read);

    le = ino->extents.Flink;
    while (le != &ino->extents) {
        extent* ext = _CR(le, extent, list_entry);

        if (ext->offset <= ino->position + to_read && ext->offset >= ino->position) {
            if (ext->extent_data.compression != 0) {
                print(L"FIXME - support compression\r\n"); // FIXME
                return EFI_UNSUPPORTED;
            }

            if (ext->extent_data.encryption != 0) {
                print(L"encryption not supported\r\n");
                return EFI_UNSUPPORTED;
            }

            if (ext->extent_data.encoding != 0) {
                print(L"other encodings not supported\r\n");
                return EFI_UNSUPPORTED;
            }

            if (ext->extent_data.type == EXTENT_TYPE_INLINE) {
                memcpy(dest, &ext->extent_data.data[pos - ext->offset], ext->extent_data.decoded_size - pos + ext->offset);
                dest += ext->extent_data.decoded_size - pos + ext->offset;
                left -= ext->extent_data.decoded_size - pos + ext->offset;
                pos = ext->extent_data.decoded_size + ext->offset;

                if (left == 0)
                    break;
            } else if (ext->extent_data.type == EXTENT_TYPE_REGULAR) {
                EXTENT_DATA2* ed2 = (EXTENT_DATA2*)&ext->extent_data.data[0];
                uint64_t size;
                uint8_t* tmp;

                if (ext->offset > pos) { // account for holes
                    if (ext->offset - pos >= left) {
                        pos = ext->offset;
                        break;
                    }

                    dest += ext->offset - pos;
                    left -= ext->offset - pos;
                    pos = ext->offset;
                }

                // FIXME - only use tmp if necessary
                // FIXME - unaligned reads

                size = ed2->num_bytes - pos + ext->offset;
                if (size > left)
                    size = sector_align(left, ino->vol->block->Media->BlockSize);

                Status = bs->AllocatePool(EfiBootServicesData, size, (void**)&tmp);
                if (EFI_ERROR(Status)) {
                    print_error(L"AllocatePool", Status);
                    return Status;
                }

                Status = read_data(ino->vol, ed2->address + ed2->offset + pos - ext->offset, size, tmp);
                if (EFI_ERROR(Status)) {
                    print_error(L"read_data", Status);
                    bs->FreePool(tmp);
                    return Status;
                }

                memcpy(dest, tmp, size);

                bs->FreePool(tmp);

                dest += size;
                pos += size;
                left -= size;

                if (left == 0)
                    break;
            }
        }

        le = le->Flink;
    }

    ino->position = pos;

    *bufsize = to_read;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_read(struct _EFI_FILE_HANDLE* File, UINTN* BufferSize, VOID* Buffer) {
    EFI_STATUS Status;
    inode* ino = _CR(File, inode, proto);

    if (!ino->inode_loaded) {
        Status = load_inode(ino);
        if (EFI_ERROR(Status)) {
            print_error(L"load_inode", Status);
            return Status;
        }
    }

    if (ino->inode_item.st_mode & __S_IFDIR)
        return read_dir(ino, BufferSize, Buffer);
    else
        return read_file(ino, BufferSize, Buffer);
}

static EFI_STATUS EFIAPI file_write(struct _EFI_FILE_HANDLE* File, UINTN* BufferSize, VOID* Buffer) {
    UNUSED(File);
    UNUSED(BufferSize);
    UNUSED(Buffer);

    return EFI_UNSUPPORTED;
}

static EFI_STATUS load_inode(inode* ino) {
    EFI_STATUS Status;
    KEY searchkey;
    traverse_ptr tp;

    searchkey.obj_id = ino->inode;
    searchkey.obj_type = TYPE_INODE_ITEM;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(ino->vol, ino->r, &tp, &searchkey);
    if (EFI_ERROR(Status)) {
        print_error(L"find_item", Status);
        return Status;
    }

    if (tp.key->obj_id != searchkey.obj_id || tp.key->obj_type != searchkey.obj_type) {
        print(L"Error finding INODE_ITEM for subvol ");
        print_hex(ino->r->id);
        print(L", inode ");
        print_hex(ino->inode);
        print(L".\r\n");

        free_traverse_ptr(&tp);

        return EFI_VOLUME_CORRUPTED;
    }

    if (tp.itemlen < sizeof(INODE_ITEM)) {
        print(L"INODE_ITEM length was ");
        print_dec(tp.itemlen);
        print(L" bytes, expected ");
        print_dec(sizeof(INODE_ITEM));
        print(L".\r\n");

        free_traverse_ptr(&tp);

        return EFI_VOLUME_CORRUPTED;
    }

    memcpy(&ino->inode_item, tp.item, sizeof(INODE_ITEM));
    ino->inode_loaded = true;

    InitializeListHead(&ino->extents);

    if (!(ino->inode_item.st_mode & __S_IFDIR)) {
        while (tp.key->obj_id == ino->inode && tp.key->obj_type <= TYPE_EXTENT_DATA) {
            if (tp.key->obj_type == TYPE_EXTENT_DATA && tp.itemlen >= offsetof(EXTENT_DATA, data[0])) {
                EXTENT_DATA* ed = (EXTENT_DATA*)tp.item;
                extent* ext;
                bool skip = false;

                if ((ed->type == EXTENT_TYPE_REGULAR || ed->type == EXTENT_TYPE_PREALLOC) &&
                    tp.itemlen < offsetof(EXTENT_DATA, data[0]) + sizeof(EXTENT_DATA2)) {
                    print(L"EXTENT_DATA was truncated\r\n");
                    free_traverse_ptr(&tp);
                    return EFI_VOLUME_CORRUPTED;
                }

                if (ed->type == EXTENT_TYPE_PREALLOC)
                    skip = true;
                else if (ed->type == EXTENT_TYPE_REGULAR) {
                    EXTENT_DATA2* ed2 = (EXTENT_DATA2*)ed->data;

                    skip = ed2->address == 0 && ed2->size == 0; // skip sparse
                }

                if (!skip) {
                    Status = bs->AllocatePool(EfiBootServicesData, offsetof(extent, extent_data) + tp.itemlen, (void**)&ext);
                    if (EFI_ERROR(Status)) {
                        print_error(L"AllocatePool", Status);
                        free_traverse_ptr(&tp);
                        return Status;
                    }

                    ext->offset = tp.key->offset;
                    memcpy(&ext->extent_data, tp.item, tp.itemlen);

                    InsertTailList(&ino->extents, &ext->list_entry);
                }
            }

            Status = next_item(ino->vol, &tp);
            if (Status == EFI_NOT_FOUND)
                break;
            else if (EFI_ERROR(Status)) {
                print_error(L"next_item", Status);
                free_traverse_ptr(&tp);
                return Status;
            }
        }
    }

    free_traverse_ptr(&tp);

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_set_position(struct _EFI_FILE_HANDLE* File, UINT64 Position) {
    EFI_STATUS Status;
    inode* ino = _CR(File, inode, proto);

    if (!ino->inode_loaded) {
        Status = load_inode(ino);
        if (EFI_ERROR(Status)) {
            print_error(L"load_inode", Status);
            return Status;
        }
    }

    if (ino->inode_item.st_mode & __S_IFDIR) {
        if (Position != 0)
            return EFI_UNSUPPORTED;

        ino->position = 0;
        ino->dir_position = ino->children.Flink;
    } else {
        if (Position == 0xffffffffffffffff)
            ino->position = ino->inode_item.st_size;
        else
            ino->position = Position;
    }

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI file_get_position(struct _EFI_FILE_HANDLE* File, UINT64* Position) {
    UNUSED(File);
    UNUSED(Position);

    print(L"file_get_position\r\n");

    // FIXME

    return EFI_UNSUPPORTED;
}

static EFI_STATUS EFIAPI file_get_info(struct _EFI_FILE_HANDLE* File, EFI_GUID* InformationType, UINTN* BufferSize, VOID* Buffer) {
    EFI_STATUS Status;
    inode* ino = _CR(File, inode, proto);
    EFI_GUID guid = EFI_FILE_INFO_ID;

    // FIXME - EFI_FILE_SYSTEM_INFO

    if (!memcmp(InformationType, &guid, sizeof(EFI_GUID))) {
        unsigned int size = offsetof(EFI_FILE_INFO, FileName[0]) + sizeof(CHAR16);
        EFI_FILE_INFO* info = (EFI_FILE_INFO*)Buffer;
        size_t bs = 0;

        if (ino->name) {
            for (int i = wcslen(ino->name); i >= 0; i--) {
                if (ino->name[i] == '\\') {
                    bs = i;
                    break;
                }
            }

            size += (wcslen(ino->name) - bs - 1) * sizeof(WCHAR);
        }

        if (*BufferSize < size) {
            *BufferSize = size;
            return EFI_BUFFER_TOO_SMALL;
        }

        if (!ino->inode_loaded) {
            Status = load_inode(ino);
            if (EFI_ERROR(Status)) {
                print_error(L"load_inode", Status);
                return Status;
            }
        }

        info->Size = size;
        info->FileSize = ino->inode_item.st_size;
        info->PhysicalSize = ino->inode_item.st_blocks;
//         info->CreateTime; // FIXME
//         info->LastAccessTime; // FIXME
//         info->ModificationTime; // FIXME
        info->Attribute = ino->inode_item.st_mode & __S_IFDIR ? EFI_FILE_DIRECTORY : 0;

        if (ino->name)
            memcpy(info->FileName, &ino->name[bs + 1], (wcslen(ino->name) - bs) * sizeof(WCHAR));
        else
            info->FileName[0] = 0;

        // FIXME - get other attributes from DOSATTRIB xattr?

        return EFI_SUCCESS;
    } else {
        print(L"Unrecognized file info GUID.\r\n");
        return EFI_UNSUPPORTED;
    }
}

static EFI_STATUS EFIAPI file_set_info(struct _EFI_FILE_HANDLE* File, EFI_GUID* InformationType, UINTN BufferSize, VOID* Buffer) {
    UNUSED(File);
    UNUSED(InformationType);
    UNUSED(BufferSize);
    UNUSED(Buffer);

    return EFI_UNSUPPORTED;
}

static EFI_STATUS file_flush(struct _EFI_FILE_HANDLE* File) {
    UNUSED(File);

    // nop

    return EFI_SUCCESS;
}

__inline static void populate_file_handle(EFI_FILE_PROTOCOL* h) {
    h->Revision = EFI_FILE_PROTOCOL_REVISION;
    h->Open = file_open;
    h->Close = file_close;
    h->Delete = file_delete;
    h->Read = file_read;
    h->Write = file_write;
    h->GetPosition = file_get_position;
    h->SetPosition = file_set_position;
    h->GetInfo = file_get_info;
    h->SetInfo = file_set_info;
    h->Flush = file_flush;
}

static EFI_STATUS EFIAPI open_volume(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* This, EFI_FILE_PROTOCOL** Root) {
    EFI_STATUS Status;
    volume* vol = _CR(This, volume, proto);
    inode* ino;

    if (!vol->chunks_loaded) {
        Status = load_chunks(vol);
        if (EFI_ERROR(Status)) {
            print_error(L"load_chunks", Status);
            return Status;
        }
    }

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(inode), (void**)&ino);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        return Status;
    }

    memset(ino, 0, sizeof(inode));

    InitializeListHead(&ino->children);

    populate_file_handle(&ino->proto);

    ino->r = vol->fsroot;
    ino->inode = SUBVOL_ROOT_INODE;
    ino->vol = vol;

    *Root = &ino->proto;

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI get_arc_name(EFI_QUIBBLE_PROTOCOL* This, char* ArcName, UINTN* ArcNameLen) {
    volume* vol = _CR(This, volume, quibble_proto);
    char* s;

    static const char prefix[] = "btrfs(";
    static const unsigned int needed_len = sizeof(prefix) - 1 + 37;

    if (*ArcNameLen < needed_len) {
        *ArcNameLen = needed_len;
        return EFI_BUFFER_TOO_SMALL;
    }

    *ArcNameLen = needed_len;

    strcpy(ArcName, prefix);
    memcpy(ArcName, prefix, sizeof(prefix) - 1);
    s = &ArcName[sizeof(prefix) - 1];

    for (unsigned int i = 0; i < 16; i++) {
        if ((vol->sb->uuid.uuid[i] >> 4) < 0xa)
            *s = (vol->sb->uuid.uuid[i] >> 4) + '0';
        else
            *s = (vol->sb->uuid.uuid[i] >> 4) + 'a' - 0xa;

        s++;

        if ((vol->sb->uuid.uuid[i] & 0xf) < 0xa)
            *s = (vol->sb->uuid.uuid[i] & 0xf) + '0';
        else
            *s = (vol->sb->uuid.uuid[i] & 0xf) + 'a' - 0xa;

        s++;

        if (i == 3 || i == 5 || i == 7 || i == 9) {
            *s = '-';
            s++;
        }
    }

    *s = ')';

    return EFI_SUCCESS;
}

static EFI_STATUS get_subvol_path(volume* vol, uint64_t subvol, LIST_ENTRY* pathbits, uint64_t* parent_subvol_num) {
    EFI_STATUS Status;
    KEY searchkey;
    traverse_ptr tp;
    ROOT_REF* rr;
    path_segment* ps;
    uint64_t dir_inode;

    searchkey.obj_id = subvol;
    searchkey.obj_type = TYPE_ROOT_BACKREF;
    searchkey.offset = 0xffffffffffffffff;

    Status = find_item(vol, vol->root_root, &tp, &searchkey);
    if (EFI_ERROR(Status)) {
        print_error(L"find_item", Status);
        return Status;
    }

    if (tp.key->obj_id != searchkey.obj_id || tp.key->obj_type != searchkey.obj_type) {
        print(L"ROOT_BACKREF not found for subvol ");
        print_hex(subvol);
        print(L".\r\n");

        free_traverse_ptr(&tp);
        return EFI_INVALID_PARAMETER;
    }

    if (tp.itemlen < sizeof(ROOT_REF) || tp.itemlen < offsetof(ROOT_REF, name[0]) + ((ROOT_REF*)tp.item)->n) {
        print(L"ROOT_BACKREF was truncated.\r\n");
        free_traverse_ptr(&tp);
        return EFI_INVALID_PARAMETER;
    }

    rr = (ROOT_REF*)tp.item;

    Status = bs->AllocatePool(EfiBootServicesData, offsetof(path_segment, name[0]) + rr->n + 1, (void**)&ps);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        free_traverse_ptr(&tp);
        return Status;
    }

    memcpy(ps->name, rr->name, rr->n);
    ps->name[rr->n] = 0;
    InsertHeadList(pathbits, &ps->list_entry);

    *parent_subvol_num = tp.key->offset;
    dir_inode = rr->dir;

    free_traverse_ptr(&tp);

    if (dir_inode != SUBVOL_ROOT_INODE) {
        LIST_ENTRY* le;
        root* parent_subvol = NULL;
        INODE_REF* ir;

        le = vol->roots.Flink;
        while (le != &vol->roots) {
            root* r2 = _CR(le, root, list_entry);

            if (r2->id == *parent_subvol_num) {
                parent_subvol = r2;
                break;
            }

            le = le->Flink;
        }

        if (!parent_subvol) {
            print(L"Could not find subvol ");
            print_hex(*parent_subvol_num);
            print(L".\r\n");
            return EFI_INVALID_PARAMETER;
        }

        do {
            searchkey.obj_id = dir_inode;
            searchkey.obj_type = TYPE_INODE_REF; // no hardlinks for directories, so should never be INODE_EXTREF
            searchkey.offset = 0xffffffffffffffff;

            Status = find_item(vol, parent_subvol, &tp, &searchkey);
            if (EFI_ERROR(Status)) {
                print_error(L"find_item", Status);
                return Status;
            }

            if (tp.key->obj_id != searchkey.obj_id || tp.key->obj_type != searchkey.obj_type) {
                print(L"INODE_REF not found for inode ");
                print_hex(searchkey.obj_id);
                print(L" in subvol ");
                print_hex(*parent_subvol_num);
                print(L".\r\n");

                free_traverse_ptr(&tp);
                return EFI_INVALID_PARAMETER;
            }

            if (tp.itemlen < sizeof(INODE_REF) || tp.itemlen < offsetof(INODE_REF, name[0]) + ((INODE_REF*)tp.item)->n) {
                print(L"INODE_REF was truncated.\r\n");
                free_traverse_ptr(&tp);
                return EFI_INVALID_PARAMETER;
            }

            ir = (INODE_REF*)tp.item;

            Status = bs->AllocatePool(EfiBootServicesData, offsetof(path_segment, name[0]) + ir->n + 1, (void**)&ps);
            if (EFI_ERROR(Status)) {
                print_error(L"AllocatePool", Status);
                free_traverse_ptr(&tp);
                return Status;
            }

            memcpy(ps->name, ir->name, ir->n);
            ps->name[ir->n] = 0;
            InsertHeadList(pathbits, &ps->list_entry);

            dir_inode = tp.key->offset;

            free_traverse_ptr(&tp);
        } while (dir_inode != SUBVOL_ROOT_INODE);
    }

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI open_subvol(EFI_OPEN_SUBVOL_PROTOCOL* This, UINT64 Subvol, EFI_FILE_HANDLE* File) {
    EFI_STATUS Status;
    volume* vol = _CR(This, volume, open_subvol_proto);
    inode* ino;
    root* r = NULL;
    LIST_ENTRY* le;
    WCHAR* name = NULL;

    if (!vol->chunks_loaded) {
        Status = load_chunks(vol);
        if (EFI_ERROR(Status)) {
            print_error(L"load_chunks", Status);
            return Status;
        }
    }

    le = vol->roots.Flink;
    while (le != &vol->roots) {
        root* r2 = _CR(le, root, list_entry);

        if (r2->id == Subvol) {
            r = r2;
            break;
        }

        le = le->Flink;
    }

    if (!r)
        return EFI_NOT_FOUND;


    if (Subvol != BTRFS_ROOT_FSTREE) {
        LIST_ENTRY pathbits;
        uint64_t root_num = Subvol;
        uint64_t parent;
        unsigned int len, left;
        WCHAR* s;

        InitializeListHead(&pathbits);

        do {
            Status = get_subvol_path(vol, root_num, &pathbits, &parent);
            if (EFI_ERROR(Status)) {
                print_error(L"get_subvol_path", Status);

                while (!IsListEmpty(&pathbits)) {
                    path_segment* ps = _CR(pathbits.Flink, path_segment, list_entry);
                    RemoveEntryList(&ps->list_entry);
                    bs->FreePool(ps);
                }

                return Status;
            }

            root_num = parent;
        } while (parent != BTRFS_ROOT_FSTREE);

        len = 0;

        le = pathbits.Flink;
        while (le != &pathbits) {
            path_segment* ps = _CR(le, path_segment, list_entry);
            unsigned int pslen;

            Status = utf8_to_utf16(NULL, 0, &pslen, ps->name, strlen(ps->name));
            if (EFI_ERROR(Status)) {
                print_error(L"utf8_to_utf16", Status);

                while (!IsListEmpty(&pathbits)) {
                    path_segment* ps = _CR(pathbits.Flink, path_segment, list_entry);
                    RemoveEntryList(&ps->list_entry);
                    bs->FreePool(ps);
                }

                return Status;
            }

            len += pslen + sizeof(WCHAR);

            le = le->Flink;
        }

        Status = bs->AllocatePool(EfiBootServicesData, len, (void**)&name);
        if (EFI_ERROR(Status)) {
            print_error(L"AllocatePool", Status);

            while (!IsListEmpty(&pathbits)) {
                path_segment* ps = _CR(pathbits.Flink, path_segment, list_entry);
                RemoveEntryList(&ps->list_entry);
                bs->FreePool(ps);
            }

            return Status;
        }

        len -= sizeof(WCHAR);

        // assemble pathbits into path

        s = name;
        left = len;

        while (!IsListEmpty(&pathbits)) {
            path_segment* ps = _CR(pathbits.Flink, path_segment, list_entry);
            unsigned int pslen;

            RemoveEntryList(&ps->list_entry);

            if (s != name) { // not first
                *s = '\\';
                s++;
                left -= sizeof(WCHAR);
            }

            Status = utf8_to_utf16(s, left, &pslen, ps->name, strlen(ps->name));
            if (EFI_ERROR(Status)) {
                print_error(L"utf8_to_utf16", Status);

                bs->FreePool(ps);
                bs->FreePool(name);

                while (!IsListEmpty(&pathbits)) {
                    path_segment* ps = _CR(pathbits.Flink, path_segment, list_entry);
                    RemoveEntryList(&ps->list_entry);
                    bs->FreePool(ps);
                }

                return Status;
            }

            s += pslen / sizeof(WCHAR);
            left -= pslen;

            bs->FreePool(ps);
        }

        name[len / sizeof(WCHAR)] = 0;
    }

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(inode), (void**)&ino);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);

        if (name)
            bs->FreePool(name);

        return Status;
    }

    memset(ino, 0, sizeof(inode));

    InitializeListHead(&ino->children);

    populate_file_handle(&ino->proto);

    ino->r = r;
    ino->inode = SUBVOL_ROOT_INODE;
    ino->vol = vol;
    ino->name = name;

    *File = &ino->proto;

    return EFI_SUCCESS;
}

static EFI_STATUS get_driver_name(EFI_QUIBBLE_PROTOCOL* This, CHAR16* DriverName, UINTN* DriverNameLen) {
    static const CHAR16 name[] = L"btrfs";

    UNUSED(This);

    if (*DriverNameLen < sizeof(name)) {
        *DriverNameLen = sizeof(name);
        return EFI_BUFFER_TOO_SMALL;
    }

    *DriverNameLen = sizeof(name);

    memcpy(DriverName, name, sizeof(name));

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI drv_start(EFI_DRIVER_BINDING_PROTOCOL* This, EFI_HANDLE ControllerHandle,
                                   EFI_DEVICE_PATH_PROTOCOL* RemainingDevicePath) {
    EFI_STATUS Status;
    EFI_GUID disk_guid = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_GUID block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID quibble_guid = EFI_QUIBBLE_PROTOCOL_GUID;
    EFI_GUID open_subvol_guid = EFI_OPEN_SUBVOL_GUID;
    EFI_BLOCK_IO_PROTOCOL* block;
    uint32_t sblen;
    superblock* sb;
    volume* vol;
    LIST_ENTRY* le;
    EFI_DISK_IO_PROTOCOL* disk_io;

    UNUSED(RemainingDevicePath);

    le = volumes.Flink;
    while (le != &volumes) {
        volume* vol = _CR(le, volume, list_entry);

        if (vol->controller == ControllerHandle) // already set up
            return EFI_SUCCESS;

        le = le->Flink;
    }

    Status = bs->OpenProtocol(ControllerHandle, &block_guid, (void**)&block, This->DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR(Status))
        return Status;

    if (block->Media->BlockSize == 0) {
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        return EFI_UNSUPPORTED;
    }

    Status = bs->OpenProtocol(ControllerHandle, &disk_guid, (void**)&disk_io, This->DriverBindingHandle,
                              ControllerHandle, EFI_OPEN_PROTOCOL_BY_DRIVER);
    if (EFI_ERROR(Status)) {
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        return Status;
    }

    // FIXME - FAT driver also claims DISK_IO 2 protocol - do we need to?

    sblen = sector_align(sizeof(superblock), block->Media->BlockSize);

    Status = bs->AllocatePool(EfiBootServicesData, sblen, (void**)&sb);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);
        return Status;
    }

    // read superblock
    // FIXME - check other superblocks?

    Status = block->ReadBlocks(block, block->Media->MediaId, superblock_addrs[0] / block->Media->BlockSize, sblen, sb);
    if (EFI_ERROR(Status)) {
        bs->FreePool(sb);
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);
        return Status;
    }

    if (sb->magic != BTRFS_MAGIC) { // not a Btrfs FS
        bs->FreePool(sb);
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);
        return EFI_UNSUPPORTED;
    }

    // FIXME - test CRC32

    Status = bs->AllocatePool(EfiBootServicesData, sizeof(volume), (void**)&vol);
    if (EFI_ERROR(Status)) {
        print_error(L"AllocatePool", Status);
        bs->FreePool(sb);
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);
        return Status;
    }

    memset(vol, 0, sizeof(volume));

    if ((sb->incompat_flags & ~COMPAT_FLAGS) != 0) {
        print(L"Cannot mount as unsupported incompat_flags (");
        print_hex(sb->incompat_flags & ~COMPAT_FLAGS);
        print(L").\r\n");
        bs->FreePool(sb);
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);
        return EFI_UNSUPPORTED;
    }

    // FIXME - check csum type (only needed if we do checksum checking)

    vol->proto.Revision = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_REVISION;
    vol->proto.OpenVolume = open_volume;

    vol->quibble_proto.GetArcName = get_arc_name;
    vol->quibble_proto.GetWindowsDriverName = get_driver_name;

    vol->open_subvol_proto.OpenSubvol = open_subvol;

    Status = bs->InstallMultipleProtocolInterfaces(&ControllerHandle, &fs_guid, &vol->proto,
                                                   &quibble_guid, &vol->quibble_proto,
                                                   &open_subvol_guid, &vol->open_subvol_proto, NULL);
    if (EFI_ERROR(Status)) {
        print_error(L"InstallMultipleProtocolInterfaces", Status);
        bs->FreePool(sb);
        bs->FreePool(vol);
        bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
        bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);
        return Status;
    }

    vol->sb = sb;
    vol->controller = ControllerHandle;
    vol->block = block;
    vol->disk_io = disk_io;

    InsertTailList(&volumes, &vol->list_entry);

    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI drv_stop(EFI_DRIVER_BINDING_PROTOCOL* This, EFI_HANDLE ControllerHandle,
                                  UINTN NumberOfChildren, EFI_HANDLE* ChildHandleBuffer) {
    EFI_GUID disk_guid = EFI_DISK_IO_PROTOCOL_GUID;
    EFI_GUID block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;

    // FIXME - free anything that needs freeing

    bs->CloseProtocol(ControllerHandle, &block_guid, This->DriverBindingHandle, ControllerHandle);
    bs->CloseProtocol(ControllerHandle, &disk_guid, This->DriverBindingHandle, ControllerHandle);

    UNUSED(NumberOfChildren);
    UNUSED(ChildHandleBuffer);

    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS Status;
    EFI_GUID guid = EFI_DRIVER_BINDING_PROTOCOL_GUID;

    systable = SystemTable;
    bs = SystemTable->BootServices;

    InitializeListHead(&volumes);

    drvbind.Supported = drv_supported;
    drvbind.Start = drv_start;
    drvbind.Stop = drv_stop;
    drvbind.Version = 0x10;
    drvbind.ImageHandle = ImageHandle;
    drvbind.DriverBindingHandle = ImageHandle;

    Status = bs->InstallProtocolInterface(&drvbind.DriverBindingHandle, &guid,
                                          EFI_NATIVE_INTERFACE, &drvbind);
    if (EFI_ERROR(Status)) {
        print_error(L"InstallProtocolInterface", Status);
        return Status;
    }

    return EFI_SUCCESS;
}
