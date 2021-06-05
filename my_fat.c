//
// Created by xi4oyu on 6/3/21.
//

#include "my_fat.h"

struct options opts;

static char *g_addr;                // 预先读入到内存里
static int g_size;                  // 内存空间大小
static uint32_t g_data_sectors;     // 数据区扇区数
struct FAT *g_fat[NUMBER_OF_FAT];   // fat 表
struct FCB *g_root_dir;             // 根目录

int fat16_format(char *addr, int size)
{
    if (size < 0 || size < HEADER_SECTORS)
        return -1;

    memset(addr, 0, size);

    struct BootRecord *boot_record = (struct BootRecord *) addr;
    struct BPB *bpb = &boot_record->bpb;
    struct EBPB *ebpb = &boot_record->ebpb;
    uint32_t secs = size / BYTES_PER_SECTOR;

    memcpy(boot_record->jmp_boot, "\xeb\x3c\x90", sizeof(boot_record->jmp_boot));  // 这里不处理，直接跳到 boot_code 处
    memcpy(boot_record->oem_id, "my_fat16", 8);
    memset(boot_record->boot_code, 0, sizeof(boot_record->boot_code));  // 引导代码，随便填
    boot_record->end_signature = 0xAA550000;

    bpb->bytes_per_sector = BYTES_PER_SECTOR;
    bpb->sectors_per_cluster = SECTORS_PER_CLUSTER;
    bpb->reserved_sector = RESERVED_SECTOR;
    bpb->number_of_fat = NUMBER_OF_FAT;
    bpb->root_entries = ROOT_ENTRIES;

    if ((secs & 0xffff0000) == 0) {
        bpb->small_sector = (uint16_t) secs;
        bpb->large_sector = 0;
    } else {
        bpb->small_sector = 0;
        bpb->large_sector = secs;
    }

    bpb->sectors_per_fat = SECTORS_PER_FAT;

    // 按照 3.5 英寸单面，每面 80 个磁道，每磁道 9 个扇区 (360 KB) 随便填
    bpb->media_descriptor = 0xf8;  // 0xf8 表示硬盘
    bpb->sectors_per_trark = 9;
    bpb->number_of_head = 1;

    bpb->hidden_sector = 0;

    ebpb->physical_drive_number = 0x80;  // 物理硬盘
    ebpb->reserved = 1;
    ebpb->extended_boot_signature = 0; //0x28;  // 0x28/0x29 给 windows 识别用
    ebpb->volume_serial_number = 0x1234;   // 随便填
    strcpy(ebpb->volume_label, "my_fat16");
    strcpy(ebpb->file_system_type, "FAT16");

    struct FAT *fat[NUMBER_OF_FAT];

    // 初始化 fat 表
    for (int i = 0; i < NUMBER_OF_FAT; i++) {
        fat[i] = (struct FAT *) (addr + (RESERVED_SECTOR + SECTORS_PER_FAT * i) * BYTES_PER_SECTOR);
    }

    for (int i = 0; i < NUMBER_OF_FAT; i++) {
        fat[i][0].cluster = 0xfff8;     // 最低字节 0xf8 和 bpb->media_descriptor 保持一致
        fat[i][1].cluster = CLUSTER_END;     // 第一个簇不使用
    }

    return 0;
}

struct FCB *find_file(struct FCB *root, uint32_t entries, const char *path, int *error_code)
{

    struct FCB *file = NULL;

    size_t path_len = strlen(path);
    char *tmp = strdup(path);
    char *name = strtok(tmp, "/");

    assert(error_code != NULL);
    *error_code = 0;

    if (name == NULL) {
        return NULL;
    }

    fuse_log(FUSE_LOG_INFO, "find_file current filename: %s\n", name);

    char *filename;
    for (size_t i = 0; i < entries; i++) {
        if (is_entry_end(&root[i]))  // 最后一项，后续的不用继续扫描了
            break;

        if (is_entry_exists(&root[i])) {
            filename = get_filename(&root[i]);
            if (strncmp(filename, name, 8) == 0) {  // 忽略扩展名
                file = &root[i];
                break;
            }
            free(filename);
        }
    }

    char *next = name + strlen(name) + 1;

//    // 跳过连续的 '/'
//    while (next < tmp + path_len && *next == '/') {
//        next++;
//    }

    if (next < tmp + path_len && *next != '\0') { // 递归调用下一层
        if (file != NULL && file->metadata & META_DIRECTORY) {
            struct FCB *ret = NULL;

            uint32_t new_entries = BYTES_PER_SECTOR * SECTORS_PER_CLUSTER / sizeof(struct FCB);
            uint32_t cur_cluster = file->first_cluster;
            struct FCB *new_root;
            int err_code = -1;

            while (is_cluster_inuse(cur_cluster) && err_code != 0 && ret == NULL) {
                new_root = (struct FCB *) get_cluster(cur_cluster);
                ret = find_file(new_root, new_entries, next, &err_code);
                cur_cluster = g_fat[0][cur_cluster].cluster;    // 下一个簇号
            }
            *error_code = err_code;
            file = ret;
        } else if (file == NULL) { // 找不到文件
            *error_code = -ENOENT;
            file = NULL;
        } else { // 不是目录不能继续往下查找
            *error_code = -ENOTDIR;
            file = NULL;
        }
    }

    if (file == NULL)
        *error_code = -ENOENT;

    free(tmp);
    return file;
}

void *my_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    cfg->kernel_cache = 1;

    g_size = DRIVE_SIZE;
    g_addr = (char *) malloc(g_size);

    if (opts.is_create) {
        fuse_log(FUSE_LOG_INFO, "init: create an memory for formatting file system..\n");
        fat16_format(g_addr, DRIVE_SIZE);
    } else {
        fuse_log(FUSE_LOG_INFO, "init: load file %s to memory\n", opts.filename);
        FILE *fp = fopen(opts.filename, "rb");
        if (fp != NULL) {

            size_t pos = 0;
            size_t n = 0;
            while (pos < g_size) {
                n = fread(g_addr + pos, 1, g_size - pos, fp);
                pos += n;
            }

            fclose(fp);
        } else {
            fuse_log(FUSE_LOG_ERR, "init: failed to load file %s\n", opts.filename);
            abort();
        }
    }

    g_data_sectors = (g_size / BYTES_PER_SECTOR) - HEADER_SECTORS;

    // 读入 FAT
    for (int i = 0; i < NUMBER_OF_FAT; i++) {
        g_fat[i] = (struct FAT *) (g_addr + (RESERVED_SECTOR + SECTORS_PER_FAT * i) * BYTES_PER_SECTOR);
    }

    g_root_dir = (struct FCB *) (g_addr + (RESERVED_SECTOR + SECTORS_PER_FAT * NUMBER_OF_FAT) * BYTES_PER_SECTOR);

    return NULL;
}

int my_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "getattr: %s\n", path);

    int res = 0;

    (void) fi;

    memset(stbuf, 0, sizeof(struct stat));
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        int err_code;
        struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);

        if (err_code != 0) {
            res = err_code;
        } else if ((file->metadata & META_VOLUME_LABEL)) {
            res = -ENOENT;
        } else if (file->metadata & META_DIRECTORY) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 1;
            stbuf->st_uid = 1000;
            stbuf->st_gid = 1000;
        } else {
            stbuf->st_mode = 0444 | S_IFREG;
            stbuf->st_nlink = 1;
            stbuf->st_uid = 1000;
            stbuf->st_gid = 1000;
            stbuf->st_size = file->size;
        }
    }

    return res;
}

int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
               struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    // 未使用的变量会报 warning
    (void) offset;
    (void) fi;
    (void) flags;
    fuse_log(FUSE_LOG_INFO, "readdir: %s\n", path);

    struct FCB *item = g_root_dir;
    uint32_t entries = ROOT_ENTRIES;
    char *filename;
    int is_root = strcmp(path, "/") == 0;

    if (!is_root) {
        int err;
        struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err);

        if (err != 0)
            return err;

        if (file == NULL)
            return -ENOENT;

        uint32_t cur_cluster = file->first_cluster;
        entries = CLUSTER_SIZE / sizeof(struct FCB);

        int stop = 0;

        while (cur_cluster >= 2 && cur_cluster <= 0xFFEF && !stop) {
            item = (struct FCB *) get_cluster(cur_cluster);

            for (size_t i = 0; i < entries; i++) {
                if (is_entry_end(&item[i])) {
                    stop = 1;  // 后面无需再遍历了
                    break;
                }

                if (is_entry_exists(&item[i]) && !(item[i].metadata & META_VOLUME_LABEL)) {
                    filename = get_filename(&item[i]);
                    fuse_log(FUSE_LOG_INFO, "readdir: %s -> %s\n", path, filename);
                    filler(buf, filename, NULL, 0, 0);
                    free(filename);
                }
            }

            cur_cluster = g_fat[0][cur_cluster].cluster;    // 下一个簇号
        }
    } else {
        // root
        for (size_t i = 0; i < entries; i++) {
//        fuse_log(FUSE_LOG_DEBUG, "first_cluster = %d\n", item[i].first_cluster);
            if (is_entry_end(&item[i])) // 后面的目录项都是空的
                break;

            if (is_entry_exists(&item[i]) && !(item[i].metadata & META_VOLUME_LABEL)) {
                filename = get_filename(&item[i]);
                fuse_log(FUSE_LOG_INFO, "readdir: %s -> %s\n", path, filename);
                filler(buf, filename, NULL, 0, 0);
                free(filename);
            }
        }
    }

    return 0;
}

int my_open(const char *path, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "open: %s\n", path);

    const struct FCB *file = NULL;

    if (strcmp("/", path) == 0)
        return 0;

    int err;
    file = find_file(g_root_dir, ROOT_ENTRIES, path, &err);

    if (err != 0)
        return err;

    if (file == NULL || (file->metadata & META_VOLUME_LABEL))
        return -ENOENT;  // 未找到文件

    return 0;  // 找到文件了
}

int my_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{

    fuse_log(FUSE_LOG_INFO, "create: %s\n", path);

    (void) mode;

    if (strcmp(path, "/") == 0)
        return -EINVAL;

    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);
    if (err_code != 0 && err_code != -ENOENT)
        return err_code;

    if (file != NULL)  // 文件已存在
        return -EEXIST;

    char *tmp = strdup(path);
    char *parent = tmp; // 父目录
    char *name = strrchr(tmp, '/');
    if (name != NULL)
        *name++ = '\0';
    else
        name = tmp;

    if (!is_filename_available(name))
        return -EINVAL;

    if (*parent == '\0') { // 根目录
        file = get_free_entry(g_root_dir, ROOT_ENTRIES);
    } else {
        struct FCB *dir_file = find_file(g_root_dir, ROOT_ENTRIES, parent, &err_code);
        if (err_code != 0)
            return err_code;

        uint32_t cur_cluster = dir_file->first_cluster;
        uint32_t entries = CLUSTER_SIZE / sizeof(struct FCB);

        struct FCB *dir = NULL;
        file = NULL;
        while (is_cluster_inuse(cur_cluster) && file == NULL) {
            dir = (struct FCB *) get_cluster(cur_cluster);
            file = get_free_entry(dir, entries);
            cur_cluster = g_fat[0][cur_cluster].cluster;
        }

        if (file == NULL) { // 给目录文件扩个容
            file = (struct FCB *)file_new_cluster(dir_file);
        }
    }

    if (!file)  // 目录项满了
        return -ENFILE;

    memset(file, 0, sizeof(struct FCB));
    memset(file->filename, ' ', MAX_FILENAME);
    memcpy(file->filename, name, strlen(name));
    file->first_cluster = CLUSTER_END;

    return 0;
}

int my_unlink(const char *path)
{
    fuse_log(FUSE_LOG_INFO, "unlink: %s\n", path);

    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);
    if (err_code != 0)
        return err_code;

    if ((file->metadata & META_VOLUME_LABEL))
        return -ENOENT;

    if ((file->metadata & META_DIRECTORY))
        return -EISDIR;

    remove_file(file);

    return 0;
}

int my_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "read: %s\n", path);

    (void) fi;

    if (strcmp(path, "/") == 0) {
        return -EISDIR;
    }

    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);

    if (err_code != 0)
        return err_code;

    if (file->metadata & META_DIRECTORY)
        return -EISDIR;

    // 因为返回值是 4 个字节的 int 类型，那么读入的字节数不能超过 int 型最大值
    // 否则返回值溢出，和负值的错误码冲突
    if (size > INT32_MAX)
        return -EINVAL;

    // 不处理读写权限
    return (int) read_file(file, buf, offset, size);
}

int my_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "write: %s\n", path);

    (void) fi;

    if (strcmp(path, "/") == 0)
        return -EISDIR;

    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);

    if (err_code != 0)
        return err_code;

    if (file->metadata & META_DIRECTORY)
        return -EISDIR;

    if (size > INT32_MAX)
        return -EINVAL;


    return (int) write_file(file, buf, offset, size);
}

int my_flush(const char *path, struct fuse_file_info *fi)
{
    return 0;
}

int my_release(const char *path, struct fuse_file_info *fi)
{
    return 0;
}

int my_truncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
    return 0;
}

int my_rename(const char *name, const char *new_name, unsigned int flags)
{
    return 0;
}

int my_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    return 0;
}

int my_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    return 0;
}

int my_statfs(const char *path, struct statvfs *sfs)
{
    fuse_log(FUSE_LOG_INFO, "statfs: %s\n", path);

    (void) path;

    memset(sfs, 0, sizeof(struct statvfs));
    sfs->f_bsize = CLUSTER_SIZE;
    sfs->f_frsize = sfs->f_bsize;
    sfs->f_blocks = DRIVE_SIZE;
    sfs->f_namemax = MAX_FILENAME;
    sfs->f_bfree = (DRIVE_SIZE - HEADER_SECTORS * BYTES_PER_SECTOR) / sfs->f_bsize;
    sfs->f_bavail = sfs->f_bfree;
    sfs->f_fsid = 0x1234;


    return 0;
}

int my_opendir(const char *path, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "opendir: %s\n", path);
    if (strcmp("/", path) == 0)
        return 0;


    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);

    if (err_code != 0)
        return err_code;

    if ((file->metadata & META_VOLUME_LABEL))
        return -ENOENT;  // 未找到文件

    return 0;  // 找到文件了
}

int my_mkdir(const char *path, mode_t mode)
{
    fuse_log(FUSE_LOG_INFO, "mkdir: %s\n", path);

    (void) mode;

    if (strcmp(path, "/") == 0)
        return -EINVAL;

    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);

    // 路径中间有不存在的目录等错误
    if (err_code != 0 && err_code != -ENOENT)
        return err_code;

    if (file != NULL)  // 文件已存在
        return -EEXIST;

    char *tmp = strdup(path);
    char *parent = tmp; // 父目录
    char *name = strrchr(tmp, '/');
    if (name != NULL)
        *name++ = '\0';
    else
        name = tmp;

    if (!is_filename_available(name))
        return -EINVAL;

    if (*parent == '\0') { // 根目录
        file = get_free_entry(g_root_dir, ROOT_ENTRIES);
    } else {
        struct FCB *dir_file = find_file(g_root_dir, ROOT_ENTRIES, parent, &err_code);
        if (err_code != 0)
            return err_code;

        uint32_t cur_cluster = dir_file->first_cluster;
        uint32_t entries = CLUSTER_SIZE / sizeof(struct FCB);

        struct FCB *dir = NULL;
        file = NULL;
        while (is_cluster_inuse(cur_cluster) && file == NULL) {
            dir = (struct FCB *) get_cluster(cur_cluster);
            file = get_free_entry(dir, entries);
            cur_cluster = g_fat[0][cur_cluster].cluster;
        }

        if (file == NULL) { // 给目录文件扩个容
            file = (struct FCB *) file_new_cluster(dir_file);
        }
    }

    if (!file)  // 目录项满了
        return -ENFILE;

    memset(file, 0, sizeof(struct FCB));
    memset(file->filename, ' ', MAX_FILENAME);
    memcpy(file->filename, name, strlen(name));
    file->first_cluster = CLUSTER_END;
    file->metadata = (file->metadata | META_DIRECTORY);

    return 0;
}

int my_rmdir(const char *path)
{
    fuse_log(FUSE_LOG_INFO, "rmdir: %s\n", path);

    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);
    if (err_code != 0)
        return err_code;

    if ((file->metadata & META_VOLUME_LABEL))
        return -ENOENT;

    if (!(file->metadata & META_DIRECTORY))
        return -ENOTDIR;

    // 目录不为空不能删除
    if (!is_directory_empty(file))
        return -ENOTEMPTY;

    remove_file(file);
    return 0;
}

int my_releasedir(const char *path, struct fuse_file_info *fi)
{

    return 0;
}

void my_destroy(void *private_data)
{
    (void) private_data;
//    return;

    fuse_log(FUSE_LOG_INFO, "store data to file %s\n", opts.filename);
    FILE *fp = fopen(opts.filename, "wb");
    if (fp != NULL) {
        fwrite(g_addr, 1, g_size, fp);
        fclose(fp);
    } else {
        fuse_log(FUSE_LOG_ERR, "failed to save data to file %s\n", opts.filename);
        abort();
    }
}

char *get_filename(const struct FCB *file)
{
    char *filename = malloc(9);
    if (filename == NULL)
        return NULL;
    memcpy(filename, file->filename, 8);

    for (int i = 0; i < 8; i++) {
        if (filename[i] == ' ') {
            filename[i] = '\0';
            break;
        }
    }

    return filename;
}

char *get_cluster(uint32_t cluster_num)
{
    // 减 2 是因为数据区的有效簇号是 2
    char *cluster = ((char *) g_root_dir + (ROOT_ENTRIES * sizeof(struct FCB)) +
                     (cluster_num - 2) * CLUSTER_SIZE);

    return cluster;
}

int is_cluster_inuse(uint32_t cluster_num)
{
    return CLUSTER_MIN <= cluster_num && cluster_num <= CLUSTER_MAX;
}

int is_entry_end(const struct FCB *fcb)
{
    return fcb->filename[0] == '\0';
}

int is_entry_exists(const struct FCB *fcb)
{
    return fcb->filename[0] != '\x20' && fcb->filename[0] != FILE_DELETE;
}

int is_filename_available(const char *filename)
{
    size_t len = strlen(filename);
    if (len > MAX_FILENAME || len == 0)
        return 0;

    for (size_t i = 0; i < len; i++) {
        if ((filename[i] >= 'a' && filename[i] <= 'z') ||
            (filename[i] >= 'A' && filename[i] <= 'Z') ||
            (filename[i] >= '0' && filename[i] <= '9') ||
            (filename[i] == '_')
                ) { // 仅支持数字字母下划线
            continue;
        } else {
            return 0;
        }
    }

    return 1;
}

struct FCB *get_free_entry(struct FCB *dir, uint32_t entries)
{
    for (size_t i = 0; i < entries; i++) {
        if (!is_entry_exists(&dir[i]) || is_entry_end(&dir[i]))
            return &dir[i];
    }

    return NULL;
}

long long read_file(const struct FCB *fcb, void *buff, uint32_t offset, uint32_t length)
{
    size_t pos = 0;

    if (offset == fcb->size)
        return 0;

    if (offset + length < offset)  // 溢出了
        return -EINVAL;

    if (length > fcb->size || offset + length > fcb->size) { // 超出文件大小
        length = fcb->size - offset;
    }

    uint16_t cur_cluster_num = fcb->first_cluster;

    assert(is_cluster_inuse(fcb->first_cluster));

    // 定位到对应偏移的簇上
    while (offset >= CLUSTER_SIZE) {
        if (is_cluster_inuse(g_fat[0][cur_cluster_num].cluster)) {
            cur_cluster_num = g_fat[0][cur_cluster_num].cluster;
            offset -= CLUSTER_SIZE;
        } else {
            return -EINVAL;  // 文件的大小不对
        }
    }

    char *src = get_cluster(cur_cluster_num) + offset;
    uint32_t n = CLUSTER_SIZE - offset;
    while (length > CLUSTER_SIZE &&
           is_cluster_inuse(cur_cluster_num)) {
        memcpy(buff + pos, src, n);
        length -= n;
        pos += n;

        cur_cluster_num = g_fat[0][cur_cluster_num].cluster;
        src = get_cluster(cur_cluster_num);
        n = CLUSTER_SIZE;
    }

    // 剩余不足一簇的数据
    memcpy(buff + pos, src, length);
    pos += length;

    return pos;
}

long long write_file(struct FCB *fcb, const void *buff, uint32_t offset, uint32_t length)
{
    abort();

    if (length == 0)
        return 0;

    if (offset + length < offset)  // 溢出了
        return -EINVAL;

    if (!is_cluster_inuse(fcb->first_cluster))
        fcb->first_cluster = get_free_cluster_num();

    if (fcb->first_cluster == CLUSTER_END) // 没有空余的空间了
        return -ENOSPC;


    uint32_t cur_cluster = fcb->first_cluster;
    while (offset >= CLUSTER_SIZE) {
        if (is_cluster_inuse(g_fat[0][cur_cluster].cluster)) {
            cur_cluster = g_fat[0][cur_cluster].cluster;
        } else {

        }

        offset -= CLUSTER_SIZE;
    }


    return 0;
}

uint32_t get_free_cluster_num()
{
    for (size_t i = 0; i < SECTORS_PER_FAT / sizeof(struct FAT); i++) {
        if (g_fat[0][i].cluster == CLUSTER_FREE) {
            g_fat[0][i].cluster = CLUSTER_END;
            return i;
        }
    }

    return CLUSTER_END;
}

int is_directory_empty(const struct FCB *file)
{
    uint32_t entries = CLUSTER_SIZE / sizeof(struct FCB);
    uint32_t cur_cluster = file->first_cluster;

    struct FCB *dir = NULL;
    int stop = 0;
    while (is_cluster_inuse(cur_cluster) && !stop) {
        dir = (struct FCB *) get_cluster(cur_cluster);
        for (size_t i = 0; i < entries; i++) {
            if (is_entry_end(&dir[i])) {
                stop = 1;
                break;
            }

            if (is_entry_exists(&dir[i]))
                return 0;
        }

        cur_cluster = g_fat[0][cur_cluster].cluster;
    }

    return 1;
}

char *file_new_cluster(struct FCB *file)
{
    // 分配新的簇，并初始化
    uint32_t new_cluster = get_free_cluster_num();
    if (new_cluster == CLUSTER_END)  // 没有空间可用了
        return NULL;
    char *p = get_cluster(new_cluster);

    if (p == NULL)
        return NULL;

    memset(p, 0, CLUSTER_SIZE);

    // 从未有簇
    if (is_cluster_inuse(file->first_cluster)) {
        uint32_t cur_cluster = file->first_cluster;

        while (is_cluster_inuse(g_fat[0][cur_cluster].cluster)) {
            cur_cluster = g_fat[0][cur_cluster].cluster;
        }
        g_fat[0][cur_cluster].cluster = new_cluster;
    } else {  // 从未分配
        file->first_cluster = new_cluster;
    }

    return p;
}

void remove_file(struct FCB *file)
{
    release_cluster(file->first_cluster);

    file->filename[0] = FILE_DELETE;
}

void release_cluster(uint32_t first_num)
{
    uint32_t next;
    if (is_cluster_inuse(first_num)) {
        next = g_fat[0][first_num].cluster;
        release_cluster(next);

        g_fat[0][first_num].cluster = CLUSTER_FREE;
    }
}
