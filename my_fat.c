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

    if (name == NULL) { // 路径是根目录的情况
        *error_code = -ENOENT;
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
            uint16_t cur_cluster = file->first_cluster;
            struct FCB *new_root;
            int err_code = -1;

            while (is_cluster_inuse(cur_cluster) && err_code != 0 && ret == NULL) {
                new_root = (struct FCB *) get_cluster(cur_cluster);

                assert(new_root != NULL);

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

    stbuf->st_uid = 0;
    stbuf->st_gid = 0;

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
            stbuf->st_mode = S_IFDIR | 0777;
            stbuf->st_nlink = 1;
        } else {
            stbuf->st_mode = 0777 | S_IFREG;
            stbuf->st_nlink = 1;
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

        uint16_t cur_cluster = file->first_cluster;
        entries = CLUSTER_SIZE / sizeof(struct FCB);

        int stop = 0;

        while (is_cluster_inuse(cur_cluster) && !stop) {
            item = (struct FCB *) get_cluster(cur_cluster);

            assert(item != NULL);

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

    struct FCB *file = NULL;

    if (strcmp("/", path) == 0)
        return 0;

    int err;
    file = find_file(g_root_dir, ROOT_ENTRIES, path, &err);

    if (err != 0)
        return err;

    if (file == NULL || (file->metadata & META_VOLUME_LABEL)) // 未找到文件
        return -ENOENT;

    int ret;
    if (fi->flags & O_TRUNC && 0 != (ret = _truncate(file, 0)))
        return ret;

    return 0;  // 找到文件了
}

int my_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "create: %s\n", path);

    (void) mode;
    (void) fi;

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

        uint16_t cur_cluster = dir_file->first_cluster;
        uint32_t entries = CLUSTER_SIZE / sizeof(struct FCB);

        struct FCB *dir = NULL;
        file = NULL;
        while (is_cluster_inuse(cur_cluster) && file == NULL) {
            dir = (struct FCB *) get_cluster(cur_cluster);

            assert(dir != NULL);

            file = get_free_entry(dir, entries);
            cur_cluster = g_fat[0][cur_cluster].cluster;
        }

        if (file == NULL) { // 给目录文件扩个容
            file = (struct FCB *) get_cluster(file_new_cluster(dir_file, 1));
        }
    }

    if (!file)  // 目录项满了
        return -ENFILE;

    memset(file, 0, sizeof(struct FCB));
    memset(file->filename, ' ', MAX_FILENAME);
    memset(file->extname, ' ', MAX_EXTNAME);
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

    int ret = (int) write_file(file, buf, offset, size);

//    if (fi->flags & O_APPEND)
//        return ret;
//
//    // 仅仅是写，则表示覆盖
//    int err;
//    if (0 != (err = _truncate(file, offset + size)))
//        return err;

    return ret;
}

int my_flush(const char *path, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "flush: %s\n", path);

    (void) fi;

    return 0;
}

int my_release(const char *path, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "release: %s\n", path);

    (void) fi;

    return 0;
}

int my_truncate(const char *path, off_t offset, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "truncate: %s\n", path);

    (void) fi;

    int err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, path, &err_code);

    if (err_code != 0)
        return err_code;

    return _truncate(file, offset);
}

int my_rename(const char *name, const char *new_name, unsigned int flags)
{
    fuse_log(FUSE_LOG_INFO, "rename: %s->%s\n", name, new_name);

    (void)flags;
    int err_code, new_err_code;
    struct FCB *file = find_file(g_root_dir, ROOT_ENTRIES, name, &err_code);
    struct FCB *new_file = find_file(g_root_dir, ROOT_ENTRIES, new_name, &new_err_code);

    if (new_file != NULL)
    {
        if ((file->metadata & META_DIRECTORY) == META_DIRECTORY && !is_directory_empty(new_file))
            return -ENOTEMPTY;
        else
        {
            memcpy(new_file, file, sizeof(struct FCB));
            remove_file(file);
            return 0;
        }
    }
    else
    {
        char *tmp = strdup(new_name);
        char *new_parent = tmp; // 父目录
        char *new_filename = strrchr(tmp, '/');
        if (new_filename != NULL)
            *new_filename++ = '\0';
        else
            new_filename = tmp;

        if (!is_filename_available(new_filename))
            return -EINVAL;

        if (*new_parent == '\0')
        { // 根目录
            new_file = get_free_entry(g_root_dir, ROOT_ENTRIES);
        }
        else
        {
            struct FCB *dir_file = find_file(g_root_dir, ROOT_ENTRIES, new_parent, &err_code);
            if (err_code != 0)
                return err_code;

            uint16_t cur_cluster = dir_file->first_cluster;
            uint32_t entries = CLUSTER_SIZE / sizeof(struct FCB);

            struct FCB *dir = NULL;
            new_file = NULL;
            while (is_cluster_inuse(cur_cluster) && new_file == NULL)
            {
                dir = (struct FCB *)get_cluster(cur_cluster);

                assert(dir != NULL);

                new_file = get_free_entry(dir, entries);
                cur_cluster = g_fat[0][cur_cluster].cluster;
            }

            if (new_file == NULL)
            { // 给目录文件扩个容
                new_file = (struct FCB *)get_cluster(file_new_cluster(dir_file, 1));
            }
        }

        if (!new_file) // 目录项满了
            return -ENFILE;

        memcpy(new_file, file, sizeof(struct FCB));
	memcpy(new_file->filename, new_filename, strlen(new_filename));
        remove_file(file);
        return 0;
    }
    return -EFAULT;
}

int my_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "chmod: %s\n", path);

    (void) mode;
    (void) fi;

    return 0;
}

int my_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    fuse_log(FUSE_LOG_INFO, "chown: %s\n", path);

    (void) uid;
    (void) gid;
    (void) fi;

    return 0;
}

int my_statfs(const char *path, struct statvfs *sfs)
{
    fuse_log(FUSE_LOG_INFO, "statfs: %s\n", path);

    (void) path;

    memset(sfs, 0, sizeof(struct statvfs));
    sfs->f_bsize = CLUSTER_SIZE;
    sfs->f_frsize = sfs->f_bsize;
    sfs->f_blocks = DRIVE_SIZE / sfs->f_bsize;
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

        uint16_t cur_cluster = dir_file->first_cluster;
        uint32_t entries = CLUSTER_SIZE / sizeof(struct FCB);

        struct FCB *dir = NULL;
        file = NULL;
        while (is_cluster_inuse(cur_cluster) && file == NULL) {
            dir = (struct FCB *) get_cluster(cur_cluster);

            assert(dir != NULL);

            file = get_free_entry(dir, entries);
            cur_cluster = g_fat[0][cur_cluster].cluster;
        }

        if (file == NULL) { // 给目录文件扩个容
            file = (struct FCB *) get_cluster(file_new_cluster(dir_file, 1));
        }
    }

    if (!file)  // 目录项满了
        return -ENFILE;

    memset(file, 0, sizeof(struct FCB));
    memset(file->filename, ' ', MAX_FILENAME + MAX_EXTNAME);
    file->first_cluster = CLUSTER_END;
    file->metadata = (file->metadata | META_DIRECTORY);

    struct FCB *item = (struct FCB *) get_cluster(file_new_cluster(file, 1));
    if (item == NULL)
        return -ENOSPC;

    // 设置当前目录 .
    memset(item[0].filename, ' ', MAX_EXTNAME + MAX_EXTNAME);
    memcpy(item[0].filename, ".", 1);
    item[0].first_cluster = file->first_cluster;
    item[0].metadata |= META_DIRECTORY;

    // 设置父目录 ..
    memcpy(&item[1], &item[0], sizeof(struct FCB));
    memcpy(item[1].filename, "..", 2);
    item[1].first_cluster = 0;

    memcpy(file->filename, name, strlen(name));
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
    if (!is_cluster_inuse(cluster_num))
        return NULL;

    // 减 2 是因为数据区的第一个有效簇号是 2
    char *cluster = ((char *) g_root_dir + (ROOT_ENTRIES * sizeof(struct FCB)) +
                     (cluster_num - 2) * CLUSTER_SIZE);

    if (cluster > g_addr + g_size)
        return NULL;

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

    if (offset == fcb->size || length == 0)
        return 0;

    if (offset + length < offset)  // 溢出了
        return -EINVAL;

    if (length > fcb->size || offset + length > fcb->size) { // 超出文件大小
        length = fcb->size - offset;
    }

    uint16_t cur_cluster_num = fcb->first_cluster;

    // 定位到对应偏移的簇上
    while (offset >= CLUSTER_SIZE) {
        assert(is_cluster_inuse(cur_cluster_num));
        cur_cluster_num = g_fat[0][cur_cluster_num].cluster;
        offset -= CLUSTER_SIZE;
    }

    char *src = get_cluster(cur_cluster_num);
    assert(src != NULL);

    src += offset;
    uint32_t n = CLUSTER_SIZE - offset;
    while (length > CLUSTER_SIZE) {
        memcpy(buff + pos, src, n);
        length -= n;
        pos += n;

        src = get_cluster(cur_cluster_num);
        assert(src != NULL);

        cur_cluster_num = g_fat[0][cur_cluster_num].cluster;
        n = CLUSTER_SIZE;
    }

    // 剩余不足一簇的数据
    memcpy(buff + pos, src, length);
    pos += length;

    return pos;
}

long long write_file(struct FCB *fcb, const void *buff, uint32_t offset, uint32_t length)
{
    if (length == 0)
        return 0;

    if (offset + length < offset)  // 溢出了
        return -EINVAL;

    // 若文件为空，写入数据后占用簇的数量
    uint32_t write_cluster_count = (offset + length + CLUSTER_SIZE - 1) / CLUSTER_SIZE;

    // 原有文件大小占用的簇的数量
    uint32_t now_cluster_count = get_cluster_count(fcb);

    // 若文件为空，写入数据后文件的大小
    uint32_t write_size = offset + length;

    // 原有文件大小
    uint32_t now_size = fcb->size;

    // 需要扩容
    if (write_cluster_count > now_cluster_count) {
        if (CLUSTER_END == file_new_cluster(fcb, write_cluster_count - now_cluster_count))
            return -ENOSPC;
    }

    // 文件大小需要更改
    if (write_size > now_size)
        fcb->size = write_size;

    uint16_t cur = fcb->first_cluster;

    // 定位到偏移对应的起始簇
    while (offset >= CLUSTER_SIZE) {
        assert(is_cluster_inuse(cur));

        cur = g_fat[0][cur].cluster;
        offset -= CLUSTER_SIZE;
    }

    size_t pos = 0;
    char *dst = get_cluster(cur);
    assert(dst != NULL);

    dst += offset;

    uint32_t n = CLUSTER_SIZE - offset;

    while (length > CLUSTER_SIZE) {
        memcpy(dst, buff + pos, n);
        length -= n;
        pos += n;

        cur = g_fat[0][cur].cluster;
        dst = get_cluster(cur);
        assert(dst != NULL);

        n = CLUSTER_SIZE;
    }

    // 剩下不足一簇的数据
    memcpy(dst, buff + pos, length);
    pos += length;

    return pos;
}

uint16_t get_free_cluster_num(uint32_t count)
{
    if (count == 0)
        return CLUSTER_END;

    uint16_t first = CLUSTER_END;

    while (count--) {
        size_t i;
        for (i = 0; i < SECTORS_PER_FAT * BYTES_PER_SECTOR / sizeof(struct FAT); i++) {
            if (g_fat[0][i].cluster == CLUSTER_FREE && get_cluster(i) != NULL) {
                g_fat[0][i].cluster = first;
                break;
            }
        }

        if (i == SECTORS_PER_FAT * BYTES_PER_SECTOR / sizeof(struct FAT)) {
            // 不足够分配所需的簇，释放之前分配的簇
            release_cluster(first);
            return CLUSTER_END;
        }

        first = i;
    }


    return first;
}

int is_directory_empty(const struct FCB *file)
{
    uint32_t entries = CLUSTER_SIZE / sizeof(struct FCB);
    uint16_t cur_cluster = file->first_cluster;

    struct FCB *dir = NULL;
    int stop = 0;
    while (is_cluster_inuse(cur_cluster) && !stop) {
        dir = (struct FCB *) get_cluster(cur_cluster);
        assert(dir != NULL);

        for (size_t i = 0; i < entries; i++) {
            if (is_entry_end(&dir[i])) {
                stop = 1;
                break;
            }

            if (dir[i].filename[0] != '.' && is_entry_exists(&dir[i]))
                return 0;
        }

        cur_cluster = g_fat[0][cur_cluster].cluster;
    }

    return 1;
}

uint16_t file_new_cluster(struct FCB *file, uint32_t count)
{
    // 分配新的簇，并初始化
    uint16_t new_cluster = get_free_cluster_num(count);
    if (new_cluster == CLUSTER_END)  // 没有空间可用了
        return CLUSTER_END;

    uint16_t cur = new_cluster;
    char *p = NULL;
    while (is_cluster_inuse(cur)) {
        p = get_cluster(cur);
        assert(p != NULL);
        memset(p, 0, CLUSTER_SIZE);

        cur = g_fat[0][cur].cluster;
    }

    if (is_cluster_inuse(file->first_cluster)) {
        cur = file->first_cluster;

        while (is_cluster_inuse(g_fat[0][cur].cluster)) {
            cur = g_fat[0][cur].cluster;
        }
        g_fat[0][cur].cluster = new_cluster;
    } else {  // 从未分配
        file->first_cluster = new_cluster;
    }

    return new_cluster;
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

uint32_t get_cluster_count(const struct FCB *file)
{
    uint32_t count = 0;
    uint16_t cur = file->first_cluster;

    while (is_cluster_inuse(cur)) {
        count++;
        cur = g_fat[0][cur].cluster;
    }

    return count;
}

int my_access(const char *path, int flags)
{
    (void) path;
    (void) flags;
    return 0;
}

int adjust_cluster_count(struct FCB *file, uint32_t new_count)
{
    uint32_t old_count = get_cluster_count(file);

    if (old_count == new_count) {
        return 0;
    } else if (old_count > new_count) { // 缩减
        uint16_t cur = file->first_cluster;
        uint16_t pre = CLUSTER_END;

        assert(cur != CLUSTER_END);
        uint32_t counter = new_count;
        while (counter > 0) {
            assert(is_cluster_inuse(cur));

            counter--;
            pre = cur;
            cur = g_fat[0][cur].cluster;
        }

        if (pre == CLUSTER_END) { // new_count = 0
            file->first_cluster = CLUSTER_END;
        } else {
            g_fat[0][pre].cluster = CLUSTER_END;
        }

        release_cluster(cur);
    } else { // 扩容
        if (CLUSTER_END == file_new_cluster(file, new_count - old_count))
            return -ENOSPC;
    }

    return 0;
}

int _truncate(struct FCB *file, off_t offset)
{
    // 超过文件长度最大值
    if (offset > UINT32_MAX)
        return -EFBIG;

    if (file->metadata & META_DIRECTORY)
        return -EISDIR;

    // 文件大小所需的簇的数量
//    uint32_t old_cluster_count = (file->size + CLUSTER_SIZE - 1) / CLUSTER_SIZE;

    // 截断后所需的簇的数量
    uint32_t new_cluster_count = (offset + CLUSTER_SIZE - 1) / CLUSTER_SIZE;

    uint32_t old_size = file->size;
    uint32_t new_size = offset;

    int ret;
    if (0 != (ret = adjust_cluster_count(file, new_cluster_count))) {
        return ret;
    }

    if (old_size == new_size) {
        return 0;
    } else if (old_size < new_size) { // 文件大小增加
        // 把后面的内容覆盖为 0
        void *null_buf = malloc(new_size - old_size);
        if (null_buf == NULL)
            return -EFAULT;

        memset(null_buf, 0, new_size - old_size);
        long long n;
        if (new_size - old_size != (n = write_file(file, null_buf, old_size, new_size - old_size))) {
            free(null_buf);
            return (int) n;
        }
        free(null_buf);
    } else { // 文件大小减少
        // 不用管
    }

    file->size = new_size;
    return 0;
}

