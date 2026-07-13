// Per-game HOME-screen forwarder: builds the program/control/meta NCAs in memory (plaintext
// sections, NCA header_key derived on-console via SPL) and installs them through ncm + ns. The
// launched game is passed to the stub via sdmc:/switch/nethersx2/forwarders/<tid>.cfg.
#include "forwarder.h"
#include "forwarder_structs.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <turbojpeg.h>

// Set from main() to the launcher's own argv[0] so forwarders chainload the real NetherSX2.nro path.
std::string g_forwarderSelfPath;

namespace {

constexpr const char *kEmuNro = "sdmc:/switch/nethersx2/NetherSX2.nro";

#define FWD_TRY(x) do { const Result _rc_ = (x); if (R_FAILED(_rc_)) return _rc_; } while (0)
#ifndef R_SUCCEED
#define R_SUCCEED() return 0
#endif

// ---- header key (SPL, keyless) -------------------------------------------------------------------
constexpr u8 HEADER_KEK_SRC[0x10] = {
    0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A};
constexpr u8 HEADER_KEY_SRC[0x20] = {
    0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
    0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2};

Result derive_header_key(u8 header_key[0x20])
{
    u8 header_kek[0x20];
    FWD_TRY(splCryptoGenerateAesKek(HEADER_KEK_SRC, 0, 0, header_kek));
    FWD_TRY(splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC, header_key));
    FWD_TRY(splCryptoGenerateAesKey(header_kek, HEADER_KEY_SRC + 0x10, header_key + 0x10));
    R_SUCCEED();
}

// ---- ns:am application-record IPC ----------------------------------------------------------------
Service g_nsAppSrv;

Result ns_app_init()
{
    FWD_TRY(nsInitialize());
    if (hosversionAtLeast(3, 0, 0))
        FWD_TRY(nsGetApplicationManagerInterface(&g_nsAppSrv));
    else
        g_nsAppSrv = *nsGetServiceSession_ApplicationManagerInterface();
    R_SUCCEED();
}

void ns_app_exit()
{
    serviceClose(&g_nsAppSrv);
    nsExit();
}

Result ns_push_application_record(u64 tid, const FwdContentStorageRecord *records, u32 count)
{
    const struct {
        u8 last_modified_event;
        u8 padding[0x7];
        u64 tid;
    } in = {0x3 /* Installed */, {0}, tid};
    return serviceDispatchIn(&g_nsAppSrv, 16, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{records, sizeof(*records) * count}});
}

Result ns_invalidate_control_cache(u64 tid)
{
    return serviceDispatchIn(&g_nsAppSrv, 404, tid);
}

// ---- in-memory buffer + build helpers ------------------------------------------------------------
constexpr u32 IVFC_MAX_LEVEL = 6;
constexpr u32 IVFC_HASH_BLOCK_SIZE = 0x4000;
constexpr u32 PFS0_EXEFS_HASH_BLOCK_SIZE = 0x10000;
constexpr u32 PFS0_META_HASH_BLOCK_SIZE = 0x1000;
constexpr u32 PFS0_PADDING_SIZE = 0x200;
constexpr u32 ROMFS_ENTRY_EMPTY = 0xFFFFFFFF;
constexpr u32 ROMFS_FILEPARTITION_OFS = 0x200;

struct BufHelper {
    BufHelper() = default;
    void write(const void *data, u64 size)
    {
        if (offset + size >= buf.size())
            buf.resize(offset + size);
        std::memcpy(buf.data() + offset, data, size);
        offset += size;
    }
    void seek(u64 where_to) { offset = where_to; }
    auto tell() const { return offset; }
    std::vector<u8> buf;
    u64 offset{};
};

struct NcaEntry {
    NcaEntry(const BufHelper &b, NcmContentType t) : data{b.buf}, type{(u8)t}
    {
        sha256CalculateHash(hash, data.data(), data.size());
    }
    const std::vector<u8> data;
    const u8 type;
    u8 hash[SHA256_HASH_SIZE];
};

struct FileEntry {
    std::string name;
    std::vector<u8> data;
};
using FileEntries = std::vector<FileEntry>;

void add_file_entry(FileEntries &entries, const char *name, const void *data, u64 size)
{
    FileEntry e;
    e.name = name;
    e.data.resize(size);
    std::memcpy(e.data.data(), data, size);
    entries.emplace_back(std::move(e));
}
u64 write_padding(BufHelper &buf, u64 off, u64 block)
{
    const u64 size = block - (off % block);
    if (size) {
        std::vector<u8> padding(size);
        buf.write(padding.data(), padding.size());
    }
    return size;
}

u32 align32(u32 offset, u32 alignment)
{
    const u32 mask = ~(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}
u64 align64(u64 offset, u64 alignment)
{
    const u64 mask = ~(u64)(alignment - 1);
    return (offset + (alignment - 1)) & mask;
}

// ---- romfs builder (hactool-style) ---------------------------------------------------------------
struct romfs_dirent_ctx {
    u32 entry_offset;
    romfs_dirent_ctx *parent;
    romfs_dirent_ctx *child;
    romfs_dirent_ctx *sibling;
    struct romfs_fent_ctx *file;
    romfs_dirent_ctx *next;
};
struct romfs_fent_ctx {
    u32 entry_offset;
    u64 offset;
    u64 size;
    romfs_dirent_ctx *parent;
    romfs_fent_ctx *sibling;
    romfs_fent_ctx *next;
};
struct romfs_ctx_t {
    romfs_fent_ctx *files;
    u64 num_dirs;
    u64 num_files;
    u64 dir_table_size;
    u64 file_table_size;
    u64 dir_hash_table_size;
    u64 file_hash_table_size;
    u64 file_partition_size;
};

romfs_dir *romfs_get_direntry(romfs_dir *directories, u32 offset)
{
    return (romfs_dir *)((u8 *)directories + offset);
}
romfs_file *romfs_get_fentry(romfs_file *files, u32 offset)
{
    return (romfs_file *)((u8 *)files + offset);
}

u32 calc_path_hash(u32 parent, const u8 *path, u32 start, u32 path_len)
{
    u32 hash = parent ^ 123456789;
    for (u32 i = 0; i < path_len; i++) {
        hash = (hash >> 5) | (hash << 27);
        hash ^= path[start + i];
    }
    return hash;
}

u32 romfs_get_hash_table_count(u32 num_entries)
{
    if (num_entries < 3)
        return 3;
    else if (num_entries < 19)
        return num_entries | 1;
    u32 count = num_entries;
    while (count % 2 == 0 || count % 3 == 0 || count % 5 == 0 || count % 7 == 0 ||
           count % 11 == 0 || count % 13 == 0 || count % 17 == 0)
        count++;
    return count;
}

void romfs_visit_dir(const FileEntries &entries, romfs_dirent_ctx *parent, romfs_ctx_t *romfs_ctx)
{
    romfs_fent_ctx *child_file_tree = nullptr;
    for (auto &e : entries) {
        auto cur_file = (romfs_fent_ctx *)calloc(1, sizeof(romfs_fent_ctx));
        romfs_ctx->num_files++;
        cur_file->parent = parent;
        cur_file->size = e.data.size();
        romfs_ctx->file_table_size += sizeof(romfs_file) + align32(e.name.length() - 1, 4);

        if (child_file_tree == nullptr) {
            cur_file->sibling = child_file_tree;
            child_file_tree = cur_file;
        } else {
            romfs_fent_ctx *prev = child_file_tree, *child = child_file_tree->sibling;
            prev->sibling = cur_file;
            cur_file->sibling = child;
        }
        if (romfs_ctx->files == nullptr) {
            cur_file->next = romfs_ctx->files;
            romfs_ctx->files = cur_file;
        } else {
            romfs_fent_ctx *prev = romfs_ctx->files, *child = romfs_ctx->files->next;
            prev->next = cur_file;
            cur_file->next = child;
        }
    }
    parent->child = nullptr;
    parent->file = child_file_tree;
}

void build_romfs_into_file(const FileEntries &entries, BufHelper &buf)
{
    auto root_ctx = (romfs_dirent_ctx *)calloc(1, sizeof(romfs_dirent_ctx));
    root_ctx->parent = root_ctx;

    romfs_ctx_t romfs_ctx{};
    romfs_ctx.dir_table_size = sizeof(romfs_dir);
    romfs_ctx.num_dirs = 1;

    romfs_visit_dir(entries, root_ctx, &romfs_ctx);
    const u32 dir_hash_count = romfs_get_hash_table_count(romfs_ctx.num_dirs);
    const u32 file_hash_count = romfs_get_hash_table_count(romfs_ctx.num_files);
    romfs_ctx.dir_hash_table_size = 4 * dir_hash_count;
    romfs_ctx.file_hash_table_size = 4 * file_hash_count;

    romfs_header header{};
    u32 entry_offset{};

    std::vector<u32> dir_hash_table(dir_hash_count, ROMFS_ENTRY_EMPTY);
    std::vector<u32> file_hash_table(file_hash_count, ROMFS_ENTRY_EMPTY);

    auto dir_table = (romfs_dir *)calloc(1, romfs_ctx.dir_table_size);
    auto file_table = (romfs_file *)calloc(1, romfs_ctx.file_table_size);

    auto cur_file = romfs_ctx.files;
    entry_offset = 0;
    for (auto &e : entries) {
        romfs_ctx.file_partition_size = align64(romfs_ctx.file_partition_size, 0x10);
        cur_file->offset = romfs_ctx.file_partition_size;
        romfs_ctx.file_partition_size += cur_file->size;
        cur_file->entry_offset = entry_offset;
        entry_offset += sizeof(romfs_file) + align32(e.name.length() - 1, 4);
        cur_file = cur_file->next;
    }

    root_ctx->entry_offset = 0x0;

    cur_file = romfs_ctx.files;
    for (auto &e : entries) {
        auto cur_entry = romfs_get_fentry(file_table, cur_file->entry_offset);
        cur_entry->parent = cur_file->parent->entry_offset;
        cur_entry->sibling = (cur_file->sibling == nullptr ? ROMFS_ENTRY_EMPTY : cur_file->sibling->entry_offset);
        cur_entry->dataOff = cur_file->offset;
        cur_entry->dataSize = cur_file->size;

        const u32 name_size = e.name.length() - 1;
        const u32 hash = calc_path_hash(cur_file->parent->entry_offset, (const u8 *)e.name.c_str(), 1, name_size);
        cur_entry->nextHash = file_hash_table[hash % file_hash_count];
        file_hash_table[hash % file_hash_count] = cur_file->entry_offset;

        cur_entry->nameLen = name_size;
        std::memcpy(cur_entry->name, e.name.c_str() + 1, name_size);

        cur_file = cur_file->next;
    }

    auto cur_dir = root_ctx;
    while (cur_dir != nullptr) {
        auto cur_entry = romfs_get_direntry(dir_table, cur_dir->entry_offset);
        cur_entry->parent = cur_dir->parent->entry_offset;
        cur_entry->sibling = cur_dir->sibling == nullptr ? ROMFS_ENTRY_EMPTY : cur_dir->sibling->entry_offset;
        cur_entry->childDir = cur_dir->child == nullptr ? ROMFS_ENTRY_EMPTY : cur_dir->child->entry_offset;
        cur_entry->childFile = cur_dir->file == nullptr ? ROMFS_ENTRY_EMPTY : cur_dir->file->entry_offset;

        const auto hash = calc_path_hash(0, 0, 0, 0);
        cur_entry->nextHash = dir_hash_table[hash % dir_hash_count];
        dir_hash_table[hash % dir_hash_count] = cur_dir->entry_offset;
        cur_entry->nameLen = 0;

        auto temp = cur_dir;
        cur_dir = cur_dir->next;
        free(temp);
    }

    header.headerSize = sizeof(header);
    header.fileHashTableSize = romfs_ctx.file_hash_table_size;
    header.fileTableSize = romfs_ctx.file_table_size;
    header.dirHashTableSize = romfs_ctx.dir_hash_table_size;
    header.dirTableSize = romfs_ctx.dir_table_size;
    header.fileDataOff = ROMFS_FILEPARTITION_OFS;
    header.dirHashTableOff = align64(romfs_ctx.file_partition_size + ROMFS_FILEPARTITION_OFS, 4);
    header.dirTableOff = header.dirHashTableOff + romfs_ctx.dir_hash_table_size;
    header.fileHashTableOff = header.dirTableOff + romfs_ctx.dir_table_size;
    header.fileTableOff = header.fileHashTableOff + romfs_ctx.file_hash_table_size;

    buf.write(&header, sizeof(header));

    cur_file = romfs_ctx.files;
    for (auto &e : entries) {
        buf.seek(cur_file->offset + ROMFS_FILEPARTITION_OFS);
        buf.write(e.data.data(), e.data.size());
        auto temp = cur_file;
        cur_file = cur_file->next;
        free(temp);
    }

    buf.seek(header.dirHashTableOff);
    buf.write(dir_hash_table.data(), romfs_ctx.dir_hash_table_size);
    buf.write(dir_table, romfs_ctx.dir_table_size);
    free(dir_table);
    buf.write(file_hash_table.data(), romfs_ctx.file_hash_table_size);
    buf.write(file_table, romfs_ctx.file_table_size);
    free(file_table);
}

std::vector<u8> romfs_build(const FileEntries &entries, u64 *out_size)
{
    BufHelper buf;
    build_romfs_into_file(entries, buf);
    buf.seek(buf.buf.size());
    *out_size = buf.tell();
    write_padding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

// ---- pfs0 + ivfc + nca section builders ----------------------------------------------------------
std::vector<u8> build_pfs0(const FileEntries &entries)
{
    BufHelper buf;
    struct Pfs0Header { u32 magic; u32 total_files; u32 string_table_size; u32 padding; } header{};
    struct Pfs0FileTable { u64 data_offset; u64 data_size; u32 name_offset; u32 padding; };
    std::vector<Pfs0FileTable> file_table(entries.size());
    std::vector<char> string_table;

    u64 string_offset{}, data_offset{};
    for (u32 i = 0; i < entries.size(); i++) {
        file_table[i].data_offset = data_offset;
        file_table[i].data_size = entries[i].data.size();
        file_table[i].name_offset = string_offset;
        file_table[i].padding = 0;
        string_table.resize(string_offset + entries[i].name.length() + 1);
        std::memcpy(string_table.data() + string_offset, entries[i].name.c_str(), entries[i].name.length() + 1);
        data_offset += entries[i].data.size();
        string_offset += entries[i].name.length() + 1;
    }
    string_table.resize((string_table.size() + 0x1F) & ~0x1F);

    header.magic = 0x30534650;
    header.total_files = entries.size();
    header.string_table_size = string_table.size();
    buf.write(&header, sizeof(header));
    buf.write(file_table.data(), sizeof(Pfs0FileTable) * file_table.size());
    buf.write(string_table.data(), string_table.size());
    for (const auto &e : entries)
        buf.write(e.data.data(), e.data.size());
    return buf.buf;
}

std::vector<u8> build_pfs0_hash_table(const std::vector<u8> &pfs0, u32 block_size)
{
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    u32 read_size = block_size;
    for (u32 i = 0; i < pfs0.size(); i += read_size) {
        if (i + read_size >= pfs0.size())
            read_size = pfs0.size() - i;
        sha256CalculateHash(hash, pfs0.data() + i, read_size);
        buf.write(hash, sizeof(hash));
    }
    return buf.buf;
}

std::vector<u8> build_master_hash(const std::vector<u8> &data)
{
    std::vector<u8> hash(SHA256_HASH_SIZE);
    sha256CalculateHash(hash.data(), data.data(), data.size());
    return hash;
}

void write_nca_padding(BufHelper &buf) { write_padding(buf, buf.tell(), 0x200); }

void nca_encrypt_header(nca::Header *header, const u8 *key)
{
    Aes128XtsContext ctx{};
    aes128XtsContextCreate(&ctx, key, key + 0x10, true);
    u8 sector{};
    for (u64 pos = 0; pos < 0xC00; pos += 0x200) {
        aes128XtsContextResetSector(&ctx, sector++, true);
        aes128XtsEncrypt(&ctx, (u8 *)header + pos, (const u8 *)header + pos, 0x200);
    }
}

void write_nca_section(nca::Header &h, u8 index, u64 start, u64 end)
{
    auto &s = h.fs_table[index];
    s.media_start_offset = start / 0x200;
    s.media_end_offset = end / 0x200;
    s._0x8[0] = 0x1;
}

void write_nca_fs_header_pfs0(nca::Header &h, u8 index, const std::vector<u8> &master_hash, u64 hash_table_size, u32 block_size)
{
    auto &fs = h.fs_header[index];
    fs.version = 0x2;
    fs.fs_type = nca::FileSystemType_PFS0;
    fs.hash_type = nca::HashType_HierarchicalSha256;
    fs.encryption_type = nca::EncryptionType_None;
    fs.hash_data.hierarchical_sha256_data.layer_count = 0x2;
    fs.hash_data.hierarchical_sha256_data.block_size = block_size;
    fs.hash_data.hierarchical_sha256_data.hash_layer.size = hash_table_size;
    std::memcpy(fs.hash_data.hierarchical_sha256_data.master_hash, master_hash.data(), master_hash.size());
    sha256CalculateHash(&h.fs_header_hash[index], &fs, sizeof(fs));
}

void write_nca_fs_header_romfs(nca::Header &h, u8 index)
{
    auto &fs = h.fs_header[index];
    fs.version = 0x2;
    fs.fs_type = nca::FileSystemType_RomFS;
    fs.hash_type = nca::HashType_HierarchicalIntegrity;
    fs.encryption_type = nca::EncryptionType_None;
    fs.hash_data.integrity_meta_info.magic = 0x43465649; // IVFC
    fs.hash_data.integrity_meta_info.version = 0x20000;
    fs.hash_data.integrity_meta_info.master_hash_size = SHA256_HASH_SIZE;
    fs.hash_data.integrity_meta_info.info_level_hash.max_layers = 0x7;
    fs.hash_data.integrity_meta_info.info_level_hash.levels[5].block_size = 0x0E; // 0x4000
    sha256CalculateHash(&h.fs_header_hash[index], &fs, sizeof(fs));
}

void write_nca_pfs0(nca::Header &h, u8 index, const FileEntries &entries, u32 block_size, BufHelper &buf)
{
    const auto pfs0 = build_pfs0(entries);
    const auto pfs0_hash_table = build_pfs0_hash_table(pfs0, block_size);
    const auto pfs0_master_hash = build_master_hash(pfs0_hash_table);

    buf.write(pfs0_hash_table.data(), pfs0_hash_table.size());
    const auto padding_size = write_padding(buf, pfs0_hash_table.size(), PFS0_PADDING_SIZE);

    h.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.offset = pfs0_hash_table.size() + padding_size;
    h.fs_header[index].hash_data.hierarchical_sha256_data.pfs0_layer.size = pfs0.size();

    buf.write(pfs0.data(), pfs0.size());
    write_nca_padding(buf);

    const auto section_start = index == 0 ? sizeof(h) : h.fs_table[index - 1].media_end_offset * 0x200;
    write_nca_section(h, index, section_start, buf.tell());
    write_nca_fs_header_pfs0(h, index, pfs0_master_hash, pfs0_hash_table.size(), block_size);
}

std::vector<u8> ivfc_create_level(const std::vector<u8> &src)
{
    BufHelper buf;
    u8 hash[SHA256_HASH_SIZE];
    u64 read_size = IVFC_HASH_BLOCK_SIZE;
    for (u32 i = 0; i < src.size(); i += read_size) {
        if (i + read_size >= src.size())
            read_size = src.size() - i;
        sha256CalculateHash(hash, src.data() + i, read_size);
        buf.write(hash, sizeof(hash));
    }
    write_padding(buf, buf.tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

void write_nca_romfs(nca::Header &h, u8 index, const FileEntries &entries, BufHelper &buf)
{
    auto &fs = h.fs_header[index];
    auto &meta_info = fs.hash_data.integrity_meta_info;
    auto &info_level_hash = meta_info.info_level_hash;

    std::vector<u8> ivfc[IVFC_MAX_LEVEL];
    ivfc[5] = romfs_build(entries, &info_level_hash.levels[5].hash_data_size);
    for (int b = 4; b >= 0; b--) {
        ivfc[b] = ivfc_create_level(ivfc[b + 1]);
        info_level_hash.levels[b].hash_data_size = ivfc[b].size();
        info_level_hash.levels[b].block_size = 0x0E;
    }
    info_level_hash.levels[0].logical_offset = 0;
    for (int i = 1; i <= 5; i++)
        info_level_hash.levels[i].logical_offset = info_level_hash.levels[i - 1].logical_offset + info_level_hash.levels[i - 1].hash_data_size;

    for (const auto &iv : ivfc)
        buf.write(iv.data(), iv.size());
    write_nca_padding(buf);

    const auto ivfc_master_hash = build_master_hash(ivfc[0]);
    std::memcpy(meta_info.master_hash, ivfc_master_hash.data(), sizeof(meta_info.master_hash));

    const auto section_start = index == 0 ? sizeof(h) : h.fs_table[index - 1].media_end_offset * 0x200;
    write_nca_section(h, index, section_start, buf.tell());
    write_nca_fs_header_romfs(h, index);
}

void write_nca_header_encrypted(nca::Header &h, u64 tid, const u8 *header_key, nca::ContentType type, BufHelper &buf)
{
    h.magic = NCA3_MAGIC;
    h.distribution_type = nca::DistributionType_System;
    h.content_type = type;
    h.program_id = tid;
    h.sdk_version = 0x000C1100;
    h.size = buf.tell();
    nca_encrypt_header(&h, header_key);
    buf.seek(0);
    buf.write(&h, sizeof(h));
}

NcaEntry create_program_nca(u64 tid, const u8 *key, const FileEntries &exefs)
{
    BufHelper buf;
    nca::Header h{};
    buf.write(&h, sizeof(h));
    write_nca_pfs0(h, 0, exefs, PFS0_EXEFS_HASH_BLOCK_SIZE, buf);
    write_nca_header_encrypted(h, tid, key, nca::ContentType_Program, buf);
    return {buf, NcmContentType_Program};
}

NcaEntry create_control_nca(u64 tid, const u8 *key, const FileEntries &romfs)
{
    BufHelper buf;
    nca::Header h{};
    buf.write(&h, sizeof(h));
    write_nca_romfs(h, 0, romfs, buf);
    write_nca_header_encrypted(h, tid, key, nca::ContentType_Control, buf);
    return {buf, NcmContentType_Control};
}

struct MetaResult {
    std::vector<u8> nca;
    u8 hash[SHA256_HASH_SIZE];
    NcmContentMetaKey key{};
    FwdContentStorageRecord record{};
    NcmContentMetaData data{};
};

// Builds the meta NCA and the in-memory content-meta records that go straight into the ncm database.
MetaResult create_meta_nca(u64 tid, const u8 *key, NcmStorageId storage_id, const std::vector<NcaEntry> &ncas)
{
    CnmtHeader cnmt_header{};
    NcmApplicationMetaExtendedHeader cnmt_extended{};
    NcmPackagedContentInfo packaged_content_info[2]{};
    u8 digest[0x20]{};

    cnmt_header.title_id = tid;
    cnmt_header.title_version = 0;
    cnmt_header.meta_type = NcmContentMetaType_Application;
    cnmt_header.meta_header.extended_header_size = sizeof(cnmt_extended);
    cnmt_header.meta_header.content_count = 0x2; // program + control
    cnmt_header.meta_header.content_meta_count = 0x1;
    cnmt_header.meta_header.attributes = 0x0;
    cnmt_header.meta_header.storage_id = storage_id;
    cnmt_extended.patch_id = cnmt_header.title_id | 0x800;

    for (u32 i = 0; i < ncas.size(); i++) {
        std::memcpy(packaged_content_info[i].hash, ncas[i].hash, sizeof(packaged_content_info[i].hash));
        std::memcpy(&packaged_content_info[i].info.content_id, ncas[i].hash, sizeof(packaged_content_info[i].info.content_id));
        packaged_content_info[i].info.content_type = ncas[i].type;
        ncmU64ToContentInfoSize(ncas[i].data.size(), &packaged_content_info[i].info);
    }

    BufHelper cnmt_buf;
    cnmt_buf.write(&cnmt_header, sizeof(cnmt_header));
    cnmt_buf.write(&cnmt_extended, sizeof(cnmt_extended));
    cnmt_buf.write(&packaged_content_info, sizeof(packaged_content_info));
    cnmt_buf.write(digest, sizeof(digest));

    FileEntries cnmt;
    char cnmt_name[34];
    std::snprintf(cnmt_name, sizeof(cnmt_name), "Application_%016lX.cnmt", tid);
    add_file_entry(cnmt, cnmt_name, cnmt_buf.buf.data(), cnmt_buf.buf.size());

    BufHelper buf;
    nca::Header h{};
    buf.write(&h, sizeof(h));
    write_nca_pfs0(h, 0, cnmt, PFS0_META_HASH_BLOCK_SIZE, buf);
    write_nca_header_encrypted(h, tid, key, nca::ContentType_Meta, buf);

    NcaEntry nca_entry{buf, NcmContentType_Meta};

    MetaResult r;
    r.nca = nca_entry.data;
    std::memcpy(r.hash, nca_entry.hash, sizeof(r.hash));

    NcmContentMetaHeader out_header = cnmt_header.meta_header;
    out_header.content_count++; // + meta itself
    out_header.storage_id = 0;

    r.key.id = cnmt_header.title_id;
    r.key.version = cnmt_header.title_version;
    r.key.type = cnmt_header.meta_type;
    r.key.install_type = NcmContentInstallType_Full;

    r.record.key = r.key;
    r.record.storage_id = storage_id;

    r.data.header = out_header;
    r.data.extended = cnmt_extended;
    std::memcpy(&r.data.infos[0].content_id, nca_entry.hash, sizeof(r.data.infos[0].content_id));
    r.data.infos[0].content_type = nca_entry.type;
    r.data.infos[0].attr = 0;
    ncmU64ToContentInfoSize(cnmt_buf.buf.size(), &r.data.infos[0]);
    r.data.infos[0].id_offset = 0;
    r.data.infos[1] = packaged_content_info[0].info;
    r.data.infos[2] = packaged_content_info[1].info;
    return r;
}

// ---- npdm + nacp patching ------------------------------------------------------------------------
bool npdm_patch_kc(std::vector<u8> &npdm, u32 off, u32 size, u32 bitmask, u32 value)
{
    const u32 pattern = BIT(bitmask) - 1;
    const u32 mask = BIT(bitmask) | pattern;
    for (u32 i = 0; i < size; i += 4) {
        u32 cup;
        std::memcpy(&cup, npdm.data() + off + i, sizeof(cup));
        if ((cup & mask) == pattern) {
            cup = value | pattern;
            std::memcpy(npdm.data() + off + i, &cup, sizeof(cup));
            return true;
        }
    }
    return false;
}

void patch_npdm(std::vector<u8> &npdm, u64 tid)
{
    npdm::Meta meta{};
    npdm::Aci0 aci0{};
    npdm::Acid acid{};
    std::memcpy(&meta, npdm.data(), sizeof(meta));
    std::memcpy(&aci0, npdm.data() + meta.aci0_offset, sizeof(aci0));
    std::memcpy(&acid, npdm.data() + meta.acid_offset, sizeof(acid));

    aci0.program_id = tid;
    acid.program_id_min = tid;
    acid.program_id_max = tid;

    // AMS >= 1.8.0 enforces the debug KAC flag.
    u64 ver{};
    if (R_SUCCEEDED(splInitialize())) {
        splGetConfig((SplConfigItem)65000 /* ExosphereVersion */, &ver);
        splExit();
    }
    ver >>= 40;
    if (ver >= MAKEHOSVERSION(1, 8, 0)) {
        npdm_patch_kc(npdm, meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 16, BIT(19));
        npdm_patch_kc(npdm, meta.acid_offset + acid.kac_offset, acid.kac_size, 16, BIT(19));
    }

    std::memcpy(npdm.data(), &meta, sizeof(meta));
    std::memcpy(npdm.data() + meta.aci0_offset, &aci0, sizeof(aci0));
    std::memcpy(npdm.data() + meta.acid_offset, &acid, sizeof(acid));
}

void patch_nacp(NacpStruct &nacp, const std::string &name, const std::string &author, u64 tid)
{
    for (auto &lang : nacp.lang) {
        if (!name.empty())
            std::strncpy(lang.name, name.c_str(), sizeof(lang.name) - 1);
        if (!author.empty())
            std::strncpy(lang.author, author.c_str(), sizeof(lang.author) - 1);
    }
    std::memset(nacp.display_version, 0, sizeof(nacp.display_version));
    std::strncpy(nacp.display_version, "1.0.0", sizeof(nacp.display_version) - 1);
    nacp.startup_user_account = 0x00;
    nacp.user_account_switch_lock = 0x00;
    nacp.add_on_content_registration_type = 0x01;
    nacp.screenshot = 0;
    nacp.video_capture = 0x2;
    nacp.logo_type = 0x2;
    nacp.logo_handling = 0x0;
    nacp.data_loss_confirmation = 0x0;
    nacp.required_network_service_license_on_launch = 0x0;
    nacp.application_error_code_category = 0;
    nacp.presence_group_id = tid;
    nacp.save_data_owner_id = tid;
    nacp.pseudo_device_id_seed = tid;
    nacp.add_on_content_base_id = tid ^ 0x1000;
    for (auto &id : nacp.local_communication_id)
        id = tid;
    nacp.play_log_policy = 0x0;
    nacp.play_log_query_capability = 0x0;
    nacp.user_account_save_data_size = 0x0;
    nacp.user_account_save_data_journal_size = 0x0;
    nacp.device_save_data_size = 0x0;
    nacp.device_save_data_journal_size = 0x0;
    nacp.user_account_save_data_size_max = 0x0;
    nacp.user_account_save_data_journal_size_max = 0x0;
    nacp.device_save_data_size_max = 0x0;
    nacp.device_save_data_journal_size_max = 0x0;
}

bool readFile(const std::string &path, std::vector<u8> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    out.resize(sz > 0 ? (size_t)sz : 0);
    bool ok = sz <= 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// Any PNG/JPG -> NACP icon: 256x256 baseline JPEG, no alpha.
bool makeNacpIcon(const std::string &src, std::vector<u8> &out)
{
    SDL_Surface *img = IMG_Load(src.c_str());
    if (!img) return false;
    SDL_Surface *rgb = SDL_CreateRGBSurfaceWithFormat(0, 256, 256, 24, SDL_PIXELFORMAT_RGB24);
    if (!rgb) { SDL_FreeSurface(img); return false; }
    SDL_FillRect(rgb, nullptr, 0);
    SDL_Rect d{0, 0, 256, 256};
    SDL_BlitScaled(img, nullptr, rgb, &d);
    SDL_FreeSurface(img);

    tjhandle tj = tjInitCompress();
    unsigned char *jpg = nullptr; unsigned long jpgSize = 0;
    int rc = tjCompress2(tj, (unsigned char *)rgb->pixels, 256, rgb->pitch, 256, TJPF_RGB,
                         &jpg, &jpgSize, TJSAMP_420, 90, 0);
    bool ok = (rc == 0) && jpgSize > 0 && jpgSize < 0x20000;
    if (ok) { out.assign(jpg, jpg + jpgSize); }
    if (jpg) tjFree(jpg);
    tjDestroy(tj);
    SDL_FreeSurface(rgb);
    return ok;
}

Result install_forwarder(const std::string &gameKey, const std::string &name, const std::string &author,
                         const std::vector<u8> &nsoData, std::vector<u8> npdmData, NacpStruct nacp,
                         const std::vector<u8> &iconJpeg)
{
    const NcmStorageId storage_id = NcmStorageId_SdCard;

    FWD_TRY(splCryptoInitialize());
    FWD_TRY(ncmInitialize());
    FWD_TRY(ns_app_init());

    u8 header_key[0x20];
    FWD_TRY(derive_header_key(header_key));

    const std::string nroPath = g_forwarderSelfPath.empty() ? kEmuNro : g_forwarderSelfPath;
    const std::string args = nroPath + " -g \"" + gameKey + "\"";

    // Deterministic homebrew-range tid from path+args, so re-creating updates the same HOME entry.
    u64 hash_data[SHA256_HASH_SIZE / sizeof(u64)];
    const std::string hash_src = nroPath + args;
    sha256CalculateHash(hash_data, hash_src.data(), hash_src.length());
    const u64 old_tid = 0x0100000000000000 | (hash_data[0] & 0x00FFFFFFFFFFF000);
    const u64 tid = 0x0500000000000000 | (hash_data[0] & 0x00FFFFFFFFFFF000);

    // The stub reads its target from this SD file, keyed by its own program id (= tid).
    {
        mkdir("sdmc:/switch/nethersx2/forwarders", 0777);
        char cfgPath[128];
        snprintf(cfgPath, sizeof(cfgPath), "sdmc:/switch/nethersx2/forwarders/%016llx.cfg", (unsigned long long)tid);
        FILE *cf = fopen(cfgPath, "wb");
        if (!cf) return MAKERESULT(299, 1);
        fwrite(nroPath.c_str(), 1, nroPath.size() + 1, cf); // "nextNroPath\0nextArgv\0"
        fwrite(args.c_str(), 1, args.size() + 1, cf);
        fclose(cf);
    }

    std::vector<NcaEntry> nca_entries;

    // program: exefs (stub main + main.npdm)
    {
        patch_npdm(npdmData, tid);
        FileEntries exefs;
        add_file_entry(exefs, "main", nsoData.data(), nsoData.size());
        add_file_entry(exefs, "main.npdm", npdmData.data(), npdmData.size());
        nca_entries.emplace_back(create_program_nca(tid, header_key, exefs));
    }

    // control: romfs (control.nacp + icon)
    {
        patch_nacp(nacp, name, author, tid);
        FileEntries romfs;
        add_file_entry(romfs, "/control.nacp", &nacp, sizeof(nacp));
        add_file_entry(romfs, "/icon_AmericanEnglish.dat", iconJpeg.data(), iconJpeg.size());
        nca_entries.emplace_back(create_control_nca(tid, header_key, romfs));
    }

    const auto meta = create_meta_nca(tid, header_key, storage_id, nca_entries);

    // write program, control and meta NCAs to placeholders + register
    {
        struct WriteItem { const u8 *data; size_t size; const u8 *hash; };
        std::vector<WriteItem> items;
        for (const auto &e : nca_entries)
            items.push_back({e.data.data(), e.data.size(), e.hash});
        items.push_back({meta.nca.data(), meta.nca.size(), meta.hash});

        NcmContentStorage cs;
        FWD_TRY(ncmOpenContentStorage(&cs, storage_id));
        for (const auto &it : items) {
            NcmContentId content_id;
            NcmPlaceHolderId placeholder_id;
            std::memcpy(&content_id, it.hash, sizeof(content_id));
            Result rc = ncmContentStorageGeneratePlaceHolderId(&cs, &placeholder_id);
            if (R_SUCCEEDED(rc)) { ncmContentStorageDeletePlaceHolder(&cs, &placeholder_id);
                rc = ncmContentStorageCreatePlaceHolder(&cs, &content_id, &placeholder_id, it.size); }
            if (R_SUCCEEDED(rc)) rc = ncmContentStorageWritePlaceHolder(&cs, &placeholder_id, 0, it.data, it.size);
            if (R_SUCCEEDED(rc)) { ncmContentStorageDelete(&cs, &content_id);
                rc = ncmContentStorageRegister(&cs, &content_id, &placeholder_id); }
            if (R_FAILED(rc)) { ncmContentStorageClose(&cs); return rc; }
        }
        ncmContentStorageClose(&cs);
    }

    // ncm content-meta database
    {
        NcmContentMetaDatabase db;
        FWD_TRY(ncmOpenContentMetaDatabase(&db, storage_id));
        Result rc = ncmContentMetaDatabaseSet(&db, &meta.key, &meta.data, sizeof(meta.data));
        if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&db);
        ncmContentMetaDatabaseClose(&db);
        FWD_TRY(rc);
    }

    // application record (remove any stale entry first)
    {
        nsDeleteApplicationCompletely(old_tid);
        nsDeleteApplicationEntity(tid);
        FWD_TRY(ns_push_application_record(tid, &meta.record, 1));
        ns_invalidate_control_cache(tid);
    }
    R_SUCCEED();
}

} // namespace

bool forwarder_create(const std::string &gameKey, const std::string &name, const std::string &author,
                      const std::string &iconImgPath, char *err, std::size_t errSize)
{
    if (err && errSize) err[0] = '\0';

    std::vector<u8> nso, npdm, nacpRaw, iconJpeg;
    if (!readFile("romfs:/fwd/hbl.nso", nso) || !readFile("romfs:/fwd/hbl.npdm", npdm) ||
        !readFile("romfs:/fwd/default.nacp", nacpRaw) || nacpRaw.size() < sizeof(NacpStruct)) {
        if (err && errSize) snprintf(err, errSize, "Forwarder assets missing.");
        return false;
    }
    if (!makeNacpIcon(iconImgPath, iconJpeg)) {
        if (err && errSize) snprintf(err, errSize, "Failed to convert icon.");
        return false;
    }

    NacpStruct nacp;
    std::memcpy(&nacp, nacpRaw.data(), sizeof(nacp));

    const Result rc = install_forwarder(gameKey, name.empty() ? "NetherSX2" : name,
                                        author.empty() ? "NetherSX2" : author, nso, npdm, nacp, iconJpeg);
    ns_app_exit();
    ncmExit();
    splCryptoExit();

    if (R_FAILED(rc)) {
        if (err && errSize) snprintf(err, errSize, "Install failed (0x%08X). Sigpatches up to date?", rc);
        return false;
    }
    return true;
}
